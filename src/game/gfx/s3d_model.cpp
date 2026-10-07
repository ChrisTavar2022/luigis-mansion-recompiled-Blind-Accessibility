// Port of Unsorted/s3dmodel and auto_03_80058850 (the .mdl renderer).
#include "game/gfx/s3d_model.h"
#include "game/system/camera.h"
#include "game/system/gx_util.h"
#include "game/unported.h"
#include "helpers/endian.h"

#include <dolphin/os.h>

#include <bit>
#include <cmath>
#include <cstring>

namespace lm::gfx {
namespace {

// ---------------------------------------------------------------------------
// Engine constants (retail .data tables)
// ---------------------------------------------------------------------------

// lbl_80338C30: LM texture format code -> GX format.
constexpr u32 kTextureFormats[] = {
    GX_TF_C4, GX_TF_C8, GX_TF_C14X2, GX_TF_I4, GX_TF_I8, GX_TF_IA4,
    GX_TF_IA8, GX_TF_RGB565, GX_TF_RGB5A3, GX_TF_RGBA8, GX_TF_CMPR,
};

// lbl_80218210: cull mode per [mirror pass][packet cull index].
constexpr GXCullMode kCullModes[2][4] = {
    {GX_CULL_NONE, GX_CULL_FRONT, GX_CULL_BACK, GX_CULL_ALL},
    {GX_CULL_ALL, GX_CULL_BACK, GX_CULL_FRONT, GX_CULL_ALL},
};

// lbl_80338D28: TEV stage presets {color a,b,c,d; alpha a,b,c,d; channel; uses texture}.
struct TevPreset {
    u8 c[4];
    u8 a[4];
    u8 chan;
    bool tex;
};
constexpr TevPreset kTevPresets[] = {
    {{GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC}, {GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA}, GX_COLOR0A0, false},
    {{GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC}, {GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA}, GX_COLOR1A1, false},
    {{GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO}, {GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA}, GX_COLOR_NULL, true},
    {{GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO}, {GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO}, GX_COLOR0A0, true},
    {{GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_RASC}, {GX_CA_ZERO, GX_CA_RASA, GX_CA_ZERO, GX_CA_TEXA}, GX_COLOR0A0, true},
    {{GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO}, {GX_CA_ZERO, GX_CA_APREV, GX_CA_RASA, GX_CA_ZERO}, GX_COLOR0A0, false},
    {{GX_CC_ZERO, GX_CC_CPREV, GX_CC_ONE, GX_CC_RASC}, {GX_CA_ZERO, GX_CA_RASA, GX_CA_ZERO, GX_CA_APREV}, GX_COLOR1A1, false},
    {{GX_CC_CPREV, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO}, {GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV}, GX_COLOR_NULL, true},
};

// lbl_80338F48: texgen source for each bump-mapped texcoord.
constexpr GXTexGenSrc kBumpTexGenSrc[] = {
    GX_TG_TEXCOORD0, GX_TG_TEXCOORD1, GX_TG_TEXCOORD2, GX_TG_TEXCOORD3,
    GX_TG_TEXCOORD4, GX_TG_TEXCOORD5, GX_TG_TEXCOORD6, GX_TG_POS,
};

// lbl_80338E68: lighting channel controls, edited by materials and game code.
struct ChanCtrl {
    GXChannelID chan;
    GXBool enable;
    GXColorSrc ambSrc;
    GXColorSrc matSrc;
    u32 lightMask;
    GXDiffuseFn diffFn;
    GXAttnFn attnFn;
    u32 _1C;
};
ChanCtrl sChan[4] = {
    {GX_COLOR0, GX_TRUE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE, 0},
    {GX_ALPHA0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE, 0},
    {GX_COLOR1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE, 0},
    {GX_ALPHA1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE, 0},
};

// lbl_803CFAB0: render state shared by the draw functions.
struct RenderState {
    u8 numChans;
    u16 nodeFlags;
    Mtx base;
    const Mtx* nodeJoint;
    bool zCompare = true;
    bool zUpdate = true;
};
RenderState sState;
Vec sNodeScale = {1.0f, 1.0f, 1.0f};  // lbl_803CFAF0
s32 sMirrorPass;                       // lbl_804D8080
bool sTevEnabled = true;               // lbl_804D86A4
const MdlPacket* sPackets;             // lbl_804D8698 (base of the current model's packets)
u8 sTexCoord, sTexMap, sTevStage;      // lbl_804D86A0 / lbl_804D86A1 / lbl_804D86A2

bool lightMapMode() { return false; }  // fn_8005DC80 (lighting manager; TODO(port))

// ---------------------------------------------------------------------------
// Matrix helpers (retail auto_03_80009540)
// ---------------------------------------------------------------------------

void zeroMtx(Mtx m) { std::memset(m, 0, sizeof(Mtx)); }  // fn_8000A2F8

// fn_8000AC48: dst += weight * (a * b)
void addWeightedProduct(f32 weight, const Mtx a, const Mtx b, Mtx dst) {
    Mtx ab;
    MTXConcat(const_cast<f32(*)[4]>(a), const_cast<f32(*)[4]>(b), ab);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            dst[r][c] += weight * ab[r][c];
        }
    }
}

// fn_8000A0D0: inverse-transpose of the 3x3 part (normal matrix). Returns false if singular.
bool normalMatrix(const Mtx m, Mtx out) {
    const f32 det = m[2][1] * (m[0][2] * m[1][0]) + m[2][2] * (m[0][0] * m[1][1]) + m[2][0] * (m[0][1] * m[1][2]) -
                    m[0][2] * (m[2][0] * m[1][1]) - m[2][2] * (m[1][0] * m[0][1]) - m[1][2] * (m[0][0] * m[2][1]);
    if (det == 0.0f) {
        return false;
    }
    const f32 inv = 1.0f / det;
    out[0][0] = inv * (m[1][1] * m[2][2] - m[2][1] * m[1][2]);
    out[1][0] = inv * -(m[0][1] * m[2][2] - m[2][1] * m[0][2]);
    out[2][0] = inv * (m[0][1] * m[1][2] - m[1][1] * m[0][2]);
    out[0][1] = inv * -(m[1][0] * m[2][2] - m[2][0] * m[1][2]);
    out[1][1] = inv * (m[0][0] * m[2][2] - m[2][0] * m[0][2]);
    out[2][1] = inv * -(m[0][0] * m[1][2] - m[1][0] * m[0][2]);
    out[0][2] = inv * (m[1][0] * m[2][1] - m[2][0] * m[1][1]);
    out[1][2] = inv * -(m[0][0] * m[2][1] - m[2][0] * m[0][1]);
    out[2][2] = inv * (m[0][0] * m[1][1] - m[1][0] * m[0][1]);
    out[0][3] = out[1][3] = out[2][3] = 0.0f;
    return true;
}

// fn_8000A330: T(t) * Rz * Ry * Rx with binary angles.
void makeRotTransMtx(Mtx dst, u32 rx, u32 ry, u32 rz, f32 tx, f32 ty, f32 tz) {
    const f32 sx = sinIdx(u16(rx)), cx = cosIdx(u16(rx));
    const f32 sy = sinIdx(u16(ry)), cy = cosIdx(u16(ry));
    const f32 sz = sinIdx(u16(rz)), cz = cosIdx(u16(rz));
    const f32 sxcz = sx * cz, sxsz = sx * sz, cxcz = cx * cz, cxsz = cx * sz;
    dst[0][0] = cy * cz;
    dst[0][1] = sy * sxcz - cxsz;
    dst[0][2] = sxsz + sy * cxcz;
    dst[0][3] = tx;
    dst[1][0] = cy * sz;
    dst[1][1] = cxcz + sy * sxsz;
    dst[1][2] = sy * cxsz - sxcz;
    dst[1][3] = ty;
    dst[2][0] = -sy;
    dst[2][1] = cy * sx;
    dst[2][2] = cy * cx;
    dst[2][3] = tz;
}

// fn_8005B57C: rotation matrix from a quaternion.
void quatToMtx(Mtx m, const Quaternion& q) {
    const f32 norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    const f32 s = norm == 0.0f ? 1.0f : 2.0f / norm;
    const f32 xs = q.x * s, ys = q.y * s, zs = q.z * s;
    const f32 wx = q.w * xs, wy = q.w * ys, wz = q.w * zs;
    const f32 xx = q.x * xs, xy = q.x * ys, xz = q.x * zs;
    const f32 yy = q.y * ys, yz = q.y * zs, zz = q.z * zs;
    m[0][0] = 1.0f - (yy + zz);
    m[1][0] = xy - wz;
    m[2][0] = xz + wy;
    m[0][1] = xy + wz;
    m[1][1] = 1.0f - (zz + xx);
    m[2][1] = yz - wx;
    m[0][2] = xz - wy;
    m[1][2] = yz + wx;
    m[2][2] = 1.0f - (xx + yy);
}

// fn_8005B2C4 / fn_8005B420: re-orthonormalize a joint so one axis aims at `target`.
void aimMatrix(Mtx m, Vec target, bool aimY) {
    Vec dir = {target.x - m[0][3], target.y - m[1][3], target.z - m[2][3]};
    Vec keep = aimY ? Vec{m[0][1], m[1][1], m[2][1]} : Vec{m[0][0], m[1][0], m[2][0]};
    VECNormalize(&dir, &dir);
    m[0][2] = dir.x;
    m[1][2] = dir.y;
    m[2][2] = dir.z;
    Vec side, up;
    if (aimY) {
        VECCrossProduct(&keep, &dir, &side);
        VECNormalize(&side, &side);
        VECCrossProduct(&dir, &side, &up);
        VECNormalize(&up, &up);
        m[0][0] = side.x, m[1][0] = side.y, m[2][0] = side.z;
        m[0][1] = up.x, m[1][1] = up.y, m[2][1] = up.z;
    } else {
        VECCrossProduct(&dir, &keep, &side);
        VECNormalize(&side, &side);
        VECCrossProduct(&side, &dir, &up);
        VECNormalize(&up, &up);
        m[0][1] = side.x, m[1][1] = side.y, m[2][1] = side.z;
        m[0][0] = up.x, m[1][0] = up.y, m[2][0] = up.z;
    }
}

// ---------------------------------------------------------------------------
// Keyframe sampling
// ---------------------------------------------------------------------------

f32 beF32(const f32* p) { return std::bit_cast<f32>(be32(std::bit_cast<u32>(*p))); }
s16 beS16(const s16* p) { return s16(be16(u16(*p))); }

// Cubic Hermite between two keys; times are in frames at 60 Hz.
f32 hermite(f32 frame, f32 t0, f32 v0, f32 outTan, f32 t1, f32 v1, f32 inTan) {
    constexpr f32 kFrameTime = 1.0f / 60.0f;
    const f32 start = t0 * kFrameTime;
    const f32 t = frame * kFrameTime - start;
    const f32 inv = 1.0f / (t1 * kFrameTime - start);
    const f32 t2 = t * t;
    const f32 inv2 = inv * inv;
    const f32 a = t2 * inv;
    const f32 t3 = t2 * t;
    const f32 b = t3 * inv2;
    const f32 h = 2.0f * t3 * (inv2 * inv) - 3.0f * t2 * inv2;
    return inTan * (b - a) + outTan * (t + (b - 2.0f * a)) + (v0 * (1.0f + h) - v1 * h);
}

// fn_8005AB04: f32 keys of {time, value, tangent[, outTangent]}.
f32 sampleF32Keys(const f32* keys, u32 count, f32 frame) {
    const u32 stride = (count & 0x8000) ? 4 : 3;
    const u32 n = count & 0x7FFF;
    const f32* k = keys;
    if (frame <= beF32(&k[0])) {
        return beF32(&k[1]);
    }
    const f32* last = keys + (n - 1) * stride;
    if (frame >= beF32(&last[0])) {
        return beF32(&last[1]);
    }
    for (u32 i = n - 1; i > 0 && !(frame < beF32(&k[stride])); i--) {
        k += stride;
    }
    const f32* next = k + stride;
    const f32 outTan = beF32(&k[stride == 4 ? 3 : 2]);
    return hermite(frame, beF32(&k[0]), beF32(&k[1]), outTan, beF32(&next[0]), beF32(&next[1]), beF32(&next[2]));
}

// fn_8005AC34: s16 rotation keys {time, value, tangent[, outTangent]}; value units are
// 360/4096 degrees. Returns a binary angle. `cursor` caches the last key used.
s16 sampleRotKeys(const s16* keys, u32 count, u16* cursor, f32 frame) {
    const u32 stride = (count & 0x8000) ? 4 : 3;
    const u32 n = count & 0x7FFF;
    u32 i = *cursor;
    const s16* k = keys + i * stride;
    auto keyTime = [](const s16* p) { return f32(beS16(p)); };
    if (frame < keyTime(k)) {
        while (i != 0) {
            k -= stride;
            i--;
            if (keyTime(k) <= frame) {
                break;
            }
        }
        if (!(keyTime(k) <= frame)) {
            return s16(beS16(&k[1]) * 16);
        }
    } else {
        while (s32(i) < s32(n) - 1 && !(frame < keyTime(k + stride))) {
            k += stride;
            i++;
        }
        if (!(frame < keyTime(k + stride)) || s32(i) >= s32(n) - 1) {
            return s16(beS16(&k[1]) * 16);
        }
    }
    *cursor = u16(i);
    constexpr f32 kUnitToDeg = 0.087890625f;  // 360 / 4096
    const s16* next = k + stride;
    const f32 v = hermite(frame, keyTime(k), f32(beS16(&k[1])) * kUnitToDeg, f32(beS16(&k[stride == 4 ? 3 : 2])),
                          keyTime(next), f32(beS16(&next[1])) * kUnitToDeg, f32(beS16(&next[2])));
    const f32 angle = 182.04445f * v;  // degrees -> binary angle
    const s32 rounded = angle < 0.0f ? s32(angle - 0.5f) : s32(angle + 0.5f);
    return s16(rounded);
}

}  // namespace

// ---------------------------------------------------------------------------
// S3DModel
// ---------------------------------------------------------------------------

void S3DModel::init(const MdlData* mdl, s32 maxMatrices) {
    mPassMode = 0;
    mMdl = mdl;
    s32 count = mdl != nullptr ? mdl->numJoints + mdl->numEnvelopes : maxMatrices;
    if (maxMatrices != 0) {
        if (count > maxMatrices) {
            OSPanic(__FILE__, __LINE__, "S3DModel: model needs more matrices than allocated");
        }
        count = maxMatrices;
    }
    mJoint.assign(count, MtxBox{});
    mView.assign(count, MtxBox{});
    mNormal.assign(count, MtxBox{});
    mPrev.assign(count, MtxBox{});
    for (s32 i = 0; i < count; i++) {
        MTXCopy(gIdentityMtx, mJoint[i].m);
        MTXCopy(gIdentityMtx, mPrev[i].m);
    }
    mCursor.assign(count, JointCursor{});
    mAnim = nullptr;
    mFrame = 1.0f;
    mBlend = 1.0f;
    mColor = {0xFF, 0xFF, 0xFF, 0xFF};
}

void S3DModel::setAnim(const KeyAnim* anim) {
    if (mAnim == anim) {
        return;
    }
    mAnim = anim;
    if (mMdl != nullptr) {
        for (u32 i = 0; i < mMdl->numJoints; i++) {
            mCursor[i].rotKey[0] = mCursor[i].rotKey[1] = mCursor[i].rotKey[2] = 0;
        }
    }
}

void S3DModel::setJointControl(u32 joint, JointControl* control) { mCursor[joint].control = control; }

// fn_8005AF0C
void S3DModel::sampleJoint(const KeyAnim* anim, u32 joint, Mtx out, f32 frame) {
    const KeyJointChannels& ch = anim->joints[joint];
    auto scale = [&](int c) {
        switch (ch.count[c]) {
        case 0: return 1.0f;
        case 1: return beF32(&anim->scales[ch.index[c]]);
        default: return sampleF32Keys(&anim->scales[ch.index[c]], ch.count[c], frame);
        }
    };
    auto rot = [&](int c) -> s32 {
        const u16 n = ch.count[3 + c];
        if (n > 1) {
            return sampleRotKeys(&anim->rotations[ch.index[3 + c]], n, &mCursor[joint].rotKey[c], frame);
        }
        return n != 0 ? s16(beS16(&anim->rotations[ch.index[3 + c]]) * 16) : 0;
    };
    auto trans = [&](int c) {
        switch (ch.count[6 + c]) {
        case 0: return 0.0f;
        case 1: return beF32(&anim->translations[ch.index[6 + c]]);
        default: return sampleF32Keys(&anim->translations[ch.index[6 + c]], ch.count[6 + c], frame);
        }
    };
    const f32 sx = scale(0), sy = scale(1), sz = scale(2);
    sNodeScale = {sx, sy, sz};
    const s32 rx = rot(0), ry = rot(1), rz = rot(2);
    const f32 tx = trans(0), ty = trans(1), tz = trans(2);
    if (anim->flags & 2) {
        makeSRTMtx(sx, sy, sz, tx, ty, tz, out, rx, ry, u32(rz));
    } else {
        makeRotTransMtx(out, u32(rx), u32(ry), u32(rz), tx, ty, tz);
    }
}

// fn_8005B660: recursive joint transform. Siblings share the parent transform;
// children inherit this node's transform.
void S3DModel::calcNode(const MdlNode* node, const Mtx parent, const Vec& parentScale, const KeyAnim* anim, f32 frame) {
    const u32 j = node->index;
    JointControl* ctrl = mCursor[j].control;
    Mtx local, world;
    Vec childScale;

    if (ctrl != nullptr) {
        if (ctrl->flags & 0x01) {
            anim = ctrl->anim;
            frame = ctrl->frame;
        }
        sampleJoint(anim, j, local, frame);
        if (!(anim->flags & 2)) {
            local[0][3] *= parentScale.x;
            local[1][3] *= parentScale.y;
            local[2][3] *= parentScale.z;
        }
        MTXConcat(const_cast<f32(*)[4]>(parent), local, world);
        MTXCopy(world, ctrl->result);
        if (ctrl->flags & 0x04) {
            aimMatrix(world, ctrl->aimTarget, true);
        }
        if (ctrl->flags & 0x02) {
            aimMatrix(world, ctrl->aimTarget, false);
        }
        if (ctrl->flags & 0x08) {
            quatToMtx(world, ctrl->quat);
        }
        if (ctrl->flags & 0x100) {
            Mtx q;
            MTXIdentity(q);
            quatToMtx(q, ctrl->quat);
            MTXConcat(world, q, world);
        }
        if (ctrl->flags & 0x40) {
            ctrl->flags = 0;
            unported::fn_8005D184_jointCallback((void*)node, mJoint[j].m);
        }
        if (ctrl->flags & 0x20) {
            Mtx r;
            makeSRTMtx(ctrl->rotScale.x, ctrl->rotScale.y, ctrl->rotScale.z, 0, 0, 0, r, ctrl->rot[0], ctrl->rot[1],
                       u32(ctrl->rot[2]));
            MTXConcat(world, r, world);
            MTXCopy(world, ctrl->result);
        }
        if (anim->flags & 2) {
            MTXCopy(world, mJoint[j].m);
            childScale = parentScale;
        } else {
            childScale = {parentScale.x * sNodeScale.x, parentScale.y * sNodeScale.y, parentScale.z * sNodeScale.z};
            Mtx s;
            MTXScale(s, childScale.x, childScale.y, childScale.z);
            MTXConcat(world, s, mJoint[j].m);
            MTXConcat(ctrl->result, s, ctrl->result);
        }
        if (ctrl->flags & 0x10) {
            Mtx r;
            makeSRTMtx(ctrl->rotScale.x, ctrl->rotScale.y, ctrl->rotScale.z, 0, 0, 0, r, ctrl->rot[0], ctrl->rot[1],
                       u32(ctrl->rot[2]));
            MTXConcat(world, r, world);
        }
    } else {
        sampleJoint(anim, j, local, frame);
        if (anim->flags & 2) {
            MTXConcat(const_cast<f32(*)[4]>(parent), local, world);
            MTXConcat(const_cast<f32(*)[4]>(parent), local, mJoint[j].m);
            childScale = parentScale;
        } else {
            childScale = {parentScale.x * sNodeScale.x, parentScale.y * sNodeScale.y, parentScale.z * sNodeScale.z};
            Mtx s;
            MTXScale(s, childScale.x, childScale.y, childScale.z);
            local[0][3] *= parentScale.x;
            local[1][3] *= parentScale.y;
            local[2][3] *= parentScale.z;
            MTXConcat(const_cast<f32(*)[4]>(parent), local, world);
            MTXConcat(world, s, mJoint[j].m);
        }
    }

    if (node->siblingRel != 0) {
        calcNode(node + node->siblingRel, parent, parentScale, anim, frame);
    }
    if (node->childRel != 0) {
        calcNode(node + node->childRel, world, childScale, anim, frame);
    }
}

// fn_80058850: skinned matrices = sum(weight * joint * inverseBind).
void S3DModel::calcEnvelopes() {
    for (u32 e = 0; e < mMdl->numEnvelopes; e++) {
        const MdlEnvelope& env = mMdl->envelopes[e];
        Mtx& dst = mJoint[mMdl->numJoints + e].m;
        zeroMtx(dst);
        for (u32 k = 0; k < env.count; k++) {
            const u16 joint = be16(env.joints[k]);
            addWeightedProduct(beF32(&env.weights[k]), mJoint[joint].m, mMdl->inverseBind[joint].m, dst);
        }
    }
}

void S3DModel::calcJoints(const Mtx base) {
    if (mMdl == nullptr) {
        return;
    }
    if (mAnim != nullptr && !mMdl->nodes.empty()) {
        static const Vec kUnitScale = {1.0f, 1.0f, 1.0f};  // lbl_8020C1C8
        calcNode(&mMdl->nodes[0], base != nullptr ? base : gIdentityMtx, kUnitScale, mAnim, mFrame);
    }
    calcEnvelopes();
}

void S3DModel::calcView(const Mtx view) {
    if (mMdl == nullptr) {
        return;
    }
    MTXCopy(const_cast<f32(*)[4]>(view), sState.base);
    const u32 count = mMdl->numJoints + mMdl->numEnvelopes;
    for (u32 i = 0; i < count; i++) {
        MTXConcat(const_cast<f32(*)[4]>(view), mJoint[i].m, mView[i].m);
        normalMatrix(mView[i].m, mNormal[i].m);
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

bool S3DModel::nodeHidden(const MdlNode* node) const {
    const JointControl* ctrl = mCursor[node->index].control;
    return ctrl != nullptr && (ctrl->flags & 0x80);
}

// fn_80059AE8 / fn_80059E00 / fn_8005A114: visit the graph, pruning hidden subtrees.
template <typename Visit>
void S3DModel::walk(const MdlNode* node, Visit&& visit) {
    if (nodeHidden(node)) {
        return;
    }
    visit(node->index);
    for (const MdlNode* next : {node->childRel ? node + node->childRel : nullptr,
                                node->siblingRel ? node + node->siblingRel : nullptr}) {
        if (next != nullptr) {
            walk(next, visit);
        }
    }
}

u8 S3DModel::materialAlphaMode(u32 material) const {
    const MdlMaterial& mat = mMdl->materials[material];
    u8 mode = mat.alphaMode;
    for (u32 s = 0; s < mat.numStages; s++) {
        const u8 flags = mMdl->samplers[mat.stages[s].sampler].flags;
        u8 m = flags & 3;
        if (flags & 4) {
            m = 0;
        }
        if (m > mode) {
            mode = m;
        }
        if ((mat.color.a * mColor.a) / 255 < 0xFF) {
            mode = 2;
        }
    }
    return mode;
}

static void setTevStage(u32 presetIndex) {  // fn_8005BB74
    const TevPreset& p = kTevPresets[presetIndex];
    u8 texCoord = GX_TEXCOORD_NULL, texMap = GX_TEXMAP_NULL;
    if (p.tex) {
        texMap = sTexMap++;
        texCoord = sTexCoord++;
    }
    const auto stage = GXTevStageID(sTevStage++);
    GXSetTevOrder(stage, GXTexCoordID(texCoord), GXTexMapID(texMap), GXChannelID(p.chan));
    GXSetTevSwapMode(stage, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevKColor(GX_KCOLOR0, GXColor{0xFF, 0xFF, 0xFF, 0xFF});
    GXSetTevKColorSel(stage, GX_TEV_KCSEL_K0);
    GXSetTevKAlphaSel(stage, GX_TEV_KASEL_K0_A);
    GXSetTevColorIn(stage, GXTevColorArg(p.c[0]), GXTevColorArg(p.c[1]), GXTevColorArg(p.c[2]), GXTevColorArg(p.c[3]));
    GXSetTevAlphaIn(stage, GXTevAlphaArg(p.a[0]), GXTevAlphaArg(p.a[1]), GXTevAlphaArg(p.a[2]), GXTevAlphaArg(p.a[3]));
    GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

// fn_8005BCC0: emboss bump mapping: the texture plus a BUMP0-shifted copy subtracted from it.
static void setBumpStages(u32 texCoordIndex) {
    const auto coord = GXTexCoordID(texCoordIndex);
    const auto map = GXTexMapID(sTexMap);
    auto stage = GXTevStageID(sTevStage);
    GXSetTexCoordGen2(coord, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXSetTevOrder(stage, coord, map, GX_COLOR0);
    GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_HALF, GX_CC_RASC);
    GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    sTevStage++;
    sTexMap++;
    const auto coord2 = GXTexCoordID(texCoordIndex + 1);
    stage = GXTevStageID(sTevStage);
    GXSetTexCoordGen2(coord2, GX_TG_BUMP0, kBumpTexGenSrc[texCoordIndex], GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXSetTevOrder(stage, coord2, map, GX_COLOR_NULL);
    GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_HALF, GX_CC_CPREV);
    GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevColorOp(stage, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    sTevStage++;
}

// fn_8005BE90: lighting stages for the first texture (or untextured).
static void setLitStages(bool textured) {
    if (!textured) {
        GXSetNumTexGens(0);
        if (sState.numChans >= 1) {
            setTevStage(0);
        }
        if (sState.numChans == 2) {
            setTevStage(1);
        }
        return;
    }
    switch (sState.numChans) {
    case 0:
        setTevStage(2);
        break;
    case 1:
        setTevStage(sChan[0]._1C + 3);
        break;
    case 2:
        setTevStage(sChan[0]._1C + 3);
        setTevStage(sChan[2]._1C + 5);
        break;
    default:
        break;
    }
}

// fn_8005BFA0: projected texture (texture stage mode 2).
static void setProjectedTexGen(const MdlTexStage& stage, const Mtx joint) {
    // TODO(port): verify the texture-matrix terms against fn_8005BFA0 (m2c could not
    // fully decode its paired-single math); used by projected-light materials.
    Mtx view;
    MTXConcat(gCamera.viewMtx, const_cast<f32(*)[4]>(joint), view);
    const f32 scaleX = stage.params[0];
    const f32 scaleY = stage.params[1];
    const s32 angle = s32((182.04445f * stage.params[2]) / 1.0f);
    const f32 c = cosIdx(u16(angle)), s = sinIdx(u16(angle));
    const f32 offX = stage.params[3], offY = stage.params[4];
    const f32 pivotX = stage.params[5], pivotY = stage.params[6];
    Mtx tex;
    MTXCopy(gIdentityMtx, tex);
    tex[0][0] = scaleX * c;
    tex[0][1] = -scaleX * s;
    tex[0][3] = offX + (pivotX + (pivotX * (-scaleX * c) + pivotY * (scaleX * s)));
    tex[1][0] = scaleY * s;
    tex[1][1] = scaleY * c;
    tex[1][3] = offY + (pivotY + (pivotX * (-scaleY * s) - pivotY * (scaleY * c)));
    GXLoadTexMtxImm(tex, GX_PTTEXMTX0, GX_MTX3x4);
    GXLoadTexMtxImm(view, GX_TEXMTX0, GX_MTX2x4);
    GXSetTexCoordGen2(GXTexCoordID(sTexCoord), GX_TG_MTX2x4, GX_TG_NRM, GX_TEXMTX0, GX_FALSE, GX_PTTEXMTX0);
}

// fn_8005C154: TEV setup for a material's texture stages.
static void setupMaterialTev(const MdlMaterial& mat, const MdlData& mdl, const Mtx joint) {
    const u8 start = lightMapMode() ? 2 : 0;
    sTexMap = sTexCoord = sTevStage = start;
    if (mat.numStages == 0) {
        setLitStages(false);
        return;
    }
    u8 extraTexCoords = 0;
    for (u32 i = 0; i < mat.numStages; i++) {
        const MdlTexStage& stage = mat.stages[i];
        if (stage.mode == 0) {
            if (mdl.samplers[stage.sampler].flags & 4) {
                extraTexCoords = 2;
                setBumpStages(mat.numStages - 1);
                continue;  // bump stages replace the lighting stage
            }
            GXSetTexCoordGen2(GXTexCoordID(sTexCoord), GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE,
                              GX_PTIDENTITY);
        } else if (stage.mode == 2) {
            setProjectedTexGen(stage, joint);
        }
        if (i != 0) {
            setTevStage(7);
        } else {
            setLitStages(true);
        }
    }
    sTexCoord += extraTexCoords;
}

void S3DModel::setupMaterial(u32 material) {
    const MdlMaterial& mat = mMdl->materials[material];
    GXColor color = mat.color;
    if (mat.numStages == 0) {
        color.r = u8((color.r * mColor.r) / 255);
        color.g = u8((color.g * mColor.g) / 255);
        color.b = u8((color.b * mColor.b) / 255);
    }
    color.a = u8((color.a * mColor.a) / 255);
    GXSetChanMatColor(GX_COLOR0A0, color);
    GXSetChanMatColor(GX_COLOR1A1, color);

    // fn_8005BAB4: channel count and material color sources.
    sState.numChans = (mat.flags & 1) ? 2 : 1;
    GXSetNumChans(sState.numChans);
    // lightChan0 selects the color channels' material source, lightChan1 the alpha channels'.
    sChan[0].matSrc = sChan[2].matSrc = mat.lightChan0 ? GX_SRC_VTX : GX_SRC_REG;
    sChan[1].matSrc = sChan[3].matSrc = mat.lightChan1 ? GX_SRC_VTX : GX_SRC_REG;

    const u8 firstMap = lightMapMode() ? 2 : 0;
    for (u32 i = 0; i < mat.numStages; i++) {
        const MdlSampler& smp = mMdl->samplers[mat.stages[i].sampler];
        const MdlTexture& tex = mMdl->textures[smp.texture];
        GXTexObj obj;
        GXInitTexObj(&obj, const_cast<u8*>(tex.pixels), tex.width, tex.height, GXTexFmt(kTextureFormats[tex.format]),
                     GXTexWrapMode(smp.wrapS), GXTexWrapMode(smp.wrapT), GX_FALSE);
        GXInitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&obj, GXTexMapID(firstMap + i));
    }

    if (sTevEnabled) {
        setupMaterialTev(mat, *mMdl, *sState.nodeJoint);
        if (lightMapMode()) {
            // Two extra stages fold in the light-map lighting (see fn_8005DD50).
            const auto s0 = GXTevStageID(sTevStage), s1 = GXTevStageID(sTevStage + 1);
            GXSetNumTevStages(sTevStage + 2);
            GXSetNumTexGens(sTexCoord);
            GXSetNumChans(2);
            GXSetTevOrder(s0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_ALPHA1);
            GXSetTevSwapMode(s0, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GXSetTevKColorSel(s0, GX_TEV_KCSEL_K0);
            GXSetTevKAlphaSel(s0, GX_TEV_KASEL_K0_A);
            GXSetTevColorIn(s0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
            GXSetTevColorOp(s0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevAlphaIn(s0, GX_CA_ZERO, GX_CA_RASA, GX_CA_A0, GX_CA_ZERO);
            GXSetTevAlphaOp(s0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVREG0);
            GXSetTevOrder(s1, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
            GXSetTevSwapMode(s1, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GXSetTevKColorSel(s1, GX_TEV_KCSEL_K3);
            GXSetTevKAlphaSel(s1, GX_TEV_KASEL_K0_A);
            GXSetTevColorIn(s1, GX_CC_CPREV, GX_CC_KONST, GX_CC_A0, GX_CC_ZERO);
            GXSetTevColorOp(s1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevKColor(GX_KCOLOR0, color);
            GXSetTevAlphaIn(s1, GX_CA_ZERO, GX_CA_APREV, GX_CA_KONST, GX_CA_ZERO);
            GXSetTevAlphaOp(s1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
        } else {
            GXSetNumTevStages(sTevStage);
            GXSetNumTexGens(sTexCoord);
        }
    }
}

void S3DModel::setupVtxDesc(const MdlShape& shape) {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    if (lightMapMode()) {
        GXSetVtxDesc(GX_VA_TEX0MTXIDX, GX_DIRECT);
        GXSetVtxDesc(GX_VA_TEX1MTXIDX, GX_DIRECT);
    } else {
        GXSetVtxDesc(GX_VA_TEX6MTXIDX, GX_DIRECT);
        GXSetVtxDesc(GX_VA_TEX7MTXIDX, GX_DIRECT);
    }
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    if (mMdl->numColors != 0) {
        GXSetVtxDesc(GX_VA_CLR0, GX_INDEX16);
    }
    if (mMdl->numTexCoords != 0) {
        GXSetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    }
    if (shape.flags & 0x02000000) {
        GXSetVtxDesc(GX_VA_NBT, GX_INDEX16);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NBT, GX_NRM_NBT, GX_F32, 0);
        return;
    }
    if (mMdl->numNormals != 0) {
        GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
    }
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
}

// fn_80059068: rebuild a billboard joint so its X axis stays horizontal relative to the view.
static void billboardMatrix(Mtx pos, Mtx nrm) {
    Vec viewUp = {sState.base[0][1], sState.base[1][1], sState.base[2][1]};
    VECNormalize(&viewUp, &viewUp);
    auto len = [](f32 x, f32 y, f32 z) { return std::sqrt(x * x + y * y + z * z); };
    const f32 xLen = len(pos[0][0], pos[1][0], pos[2][0]);
    const f32 zLen = len(pos[0][2], pos[1][2], pos[2][2]);
    Vec yAxis = {pos[0][1], pos[1][1], pos[2][1]};
    Vec xAxis;
    VECCrossProduct(&yAxis, &viewUp, &xAxis);
    if (xAxis.x * xAxis.x + xAxis.y * xAxis.y + xAxis.z * xAxis.z < 1e-6f) {
        return;
    }
    VECNormalize(&xAxis, &xAxis);
    pos[0][0] = xLen * xAxis.x, pos[1][0] = xLen * xAxis.y, pos[2][0] = xLen * xAxis.z;
    pos[0][2] = zLen * viewUp.x, pos[1][2] = zLen * viewUp.y, pos[2][2] = zLen * viewUp.z;
    const f32 nxLen = len(nrm[0][0], nrm[1][0], nrm[2][0]);
    const f32 nzLen = len(nrm[0][2], nrm[1][2], nrm[2][2]);
    nrm[0][0] = nxLen * xAxis.x, nrm[1][0] = nxLen * xAxis.y, nrm[2][0] = nxLen * xAxis.z;
    nrm[0][2] = nzLen * viewUp.x, nrm[1][2] = nzLen * viewUp.y, nrm[2][2] = nzLen * viewUp.z;
}

void S3DModel::drawPacket(const MdlPacket& packet) {
    for (u32 i = 0; i < packet.numMatrices; i++) {
        const u16 m = packet.matrixIndices[i];
        if (m == 0xFFFF) {
            continue;
        }
        const u32 slot = i * 3;
        if ((sState.nodeFlags & 3) == 2) {
            billboardMatrix(mView[m].m, mNormal[m].m);
        }
        GXLoadPosMtxImm(mView[m].m, slot);
        GXLoadNrmMtxImm(mNormal[m].m, slot);
    }
    GXSetCullMode(kCullModes[sMirrorPass][packet.cullMode & 3]);
    GXCallDisplayList(const_cast<u8*>(packet.displayList), packet.displayListSize);
}

void S3DModel::drawShape(u32 shapeIndex) {
    const MdlShape& shape = mMdl->shapes[shapeIndex];
    const GXBool lit = (shape.flags & 0x01000000) ? GX_TRUE : GX_FALSE;
    for (ChanCtrl& c : sChan) {
        c.enable = lit;
    }
    GXSetChanCtrl(GX_COLOR0, sChan[0].enable, sChan[0].ambSrc, sChan[0].matSrc, sChan[0].lightMask,
                  sChan[0].diffFn, sChan[0].attnFn);
    GXSetChanCtrl(GX_ALPHA0, sChan[1].enable, sChan[1].ambSrc, sChan[1].matSrc, sChan[1].lightMask,
                  sChan[1].diffFn, sChan[1].attnFn);
    GXSetChanCtrl(GX_COLOR1, sChan[2].enable, sChan[2].ambSrc, sChan[2].matSrc, sChan[2].lightMask,
                  sChan[2].diffFn, GX_AF_SPEC);
    GXSetChanCtrl(GX_ALPHA1, sChan[3].enable, sChan[3].ambSrc, sChan[3].matSrc, sChan[3].lightMask,
                  sChan[3].diffFn, sChan[3].attnFn);
    for (u32 i = 0; i < shape.numPackets; i++) {
        drawPacket(mMdl->packets[shape.firstPacket + i]);
    }
}

void S3DModel::drawNodeOpaque(u32 nodeIndex) {
    const MdlNode& node = mMdl->nodes[nodeIndex];
    sState.nodeFlags = node.renderFlags;
    sState.nodeJoint = &mJoint[nodeIndex].m;
    for (u32 d = 0; d < node.drawCount; d++) {
        const MdlDraw& draw = mMdl->draws[node.firstDraw + d];
        if (mMdl->materials[draw.material].flags & 2) {
            continue;
        }
        const u8 mode = materialAlphaMode(draw.material);
        if (mode == 2) {
            continue;
        }
        setupMaterial(draw.material);
        GXSetZCompLoc(mode == 0 ? GX_TRUE : GX_FALSE);
        setupVtxDesc(mMdl->shapes[draw.shape]);
        drawShape(draw.shape);
    }
}

void S3DModel::drawNodeTranslucent(u32 nodeIndex) {
    const MdlNode& node = mMdl->nodes[nodeIndex];
    sState.nodeFlags = node.renderFlags;
    sState.nodeJoint = &mJoint[nodeIndex].m;
    for (u32 d = 0; d < node.drawCount; d++) {
        const MdlDraw& draw = mMdl->draws[node.firstDraw + d];
        if (!(mMdl->materials[draw.material].flags & 2) && materialAlphaMode(draw.material) == 2) {
            setupMaterial(draw.material);
            setupVtxDesc(mMdl->shapes[draw.shape]);
            drawShape(draw.shape);
        }
    }
}

void S3DModel::drawNodeShadow(u32 nodeIndex) {
    const MdlNode& node = mMdl->nodes[nodeIndex];
    sState.nodeJoint = &mView[nodeIndex].m;
    for (u32 d = 0; d < node.drawCount; d++) {
        const MdlDraw& draw = mMdl->draws[node.firstDraw + d];
        if (mMdl->materials[draw.material].flags & 2) {
            continue;
        }
        if (mPassMode == 0) {
            GXSetZCompLoc(GX_TRUE);
            drawShape(draw.shape);
        } else if (mPassMode > 0 && mPassMode < 3) {
            GXSetZCompLoc(GX_FALSE);
            drawShape(draw.shape);
        }
    }
}

void S3DModel::setupArrays() {
    sPackets = mMdl->packets.data();
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    const u32 posBytes = u32(static_cast<const u8*>(mMdl->normals) - static_cast<const u8*>(mMdl->positions));
    const u32 end = mMdl->size;
    auto remaining = [&](const void* p) { return end - u32(static_cast<const u8*>(p) - mMdl->raw); };
    GXSetArray(GX_VA_POS, mMdl->positions, posBytes, 12, false);
    GXSetArray(GX_VA_NRM, mMdl->normals, remaining(mMdl->normals), 12, false);
    GXSetArray(GX_VA_NBT, mMdl->normals, remaining(mMdl->normals), 12, false);
    GXSetArray(GX_VA_CLR0, mMdl->colors, remaining(mMdl->colors), 4, false);
    GXSetArray(GX_VA_TEX0, mMdl->texCoords, remaining(mMdl->texCoords), 8, false);
}

void S3DModel::drawOpaque() {
    if (mMdl == nullptr || mAnim == nullptr || mColor.a == 0 || mMdl->nodes.empty()) {
        return;
    }
    setupArrays();
    GXSetZMode(sState.zCompare, GX_LEQUAL, sState.zUpdate);
    walk(&mMdl->nodes[0], [this](u32 n) { drawNodeOpaque(n); });
}

void S3DModel::drawTranslucent() {
    if (mMdl == nullptr || mColor.a == 0 || mMdl->nodes.empty()) {
        return;
    }
    setupArrays();
    GXSetZCompLoc(GX_FALSE);
    GXSetZMode(sState.zCompare, GX_LEQUAL, sState.zUpdate);
    walk(&mMdl->nodes[0], [this](u32 n) { drawNodeTranslucent(n); });
}

void S3DModel::drawShadow() {
    if (mMdl == nullptr || mColor.a == 0 || mMdl->nodes.empty()) {
        return;
    }
    setupArrays();
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_CLR0, GX_INDEX8);
    for (int c = 0; c < 4; c++) {
        sChan[c].enable = GX_FALSE;
        sChan[c].ambSrc = GX_SRC_REG;
        sChan[c].matSrc = GX_SRC_REG;
        sChan[c].lightMask = GX_LIGHT_NULL;
        sChan[c].diffFn = GX_DF_NONE;
        sChan[c].attnFn = GX_AF_NONE;
    }
    constexpr GXColor kShadow = {0, 0, 0, 0xFF};  // lbl_804DA52C
    GXSetChanMatColor(GX_COLOR0A0, kShadow);
    GXSetChanAmbColor(GX_COLOR0A0, kShadow);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevColor(GX_TEVREG0, kShadow);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetZMode(sState.zCompare, GX_LEQUAL, sState.zUpdate);
    walk(&mMdl->nodes[0], [this](u32 n) { drawNodeShadow(n); });
}

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

void beginModelDraw() {
    unported::fn_8005DD50_setupLightMapStages();
    setModelTevEnabled(true);
    GXSetDstAlpha(GX_FALSE, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_GREATER, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
}

void setMirrorPass(bool mirrored) { sMirrorPass = mirrored ? 1 : 0; }
void setModelTevEnabled(bool on) { sTevEnabled = on; }
void setModelZMode(bool compare, bool update) {
    sState.zCompare = compare;
    sState.zUpdate = update;
}

}  // namespace lm::gfx
