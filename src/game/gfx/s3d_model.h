#pragma once
// S3DModel: an instance of an .mdl model with its own joint matrices
// (retail: Unsorted/s3dmodel + auto_03_80058850, 0x80058368..0x8005C2C8).
//
// Per frame the owner calls:
//   setAnim()/setFrame()      choose the joint animation and time
//   calcJoints(base)          fn_800589B0: joint + skinning matrices in model space
//   calcView(view)            fn_80059710: view-space position/normal matrices
//   drawOpaque()/drawTranslucent()  fn_80059C54 / fn_80059F6C
#include "game/gfx/mdl.h"

#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <vector>

namespace lm::gfx {

// Per-joint overrides set by game code (head tracking, procedural rotation...).
// Retail keeps a pointer to one of these per joint (S3DModel::unk28[i].unk8).
struct JointControl {
    u32 flags;          // 0x01 own anim, 0x02 aim-X, 0x04 aim-Y, 0x08 quat, 0x10 post-rotate,
                        // 0x20 rotate, 0x40 one-shot callback, 0x80 hidden, 0x100 quat (post)
    const KeyAnim* anim;  // 0x04 (flag 0x01)
    f32 frame;            // 0x08 (flag 0x01)
    u32 _C;
    Vec aimTarget;        // 0x10
    s32 rot[3];           // 0x1C binary angles (flags 0x10/0x20)
    Vec rotScale;         // 0x28
    Quaternion quat;      // 0x34
    Mtx result;           // 0x44 final joint matrix (read back by game code)
};

class S3DModel {
public:
    S3DModel() = default;
    S3DModel(const MdlData* mdl, s32 maxMatrices = 0) { init(mdl, maxMatrices); }

    void init(const MdlData* mdl, s32 maxMatrices);           // init__8S3DModelFll

    void setAnim(const KeyAnim* anim);                         // fn_8005AA3C
    void setFrame(f32 frame) { mFrame = frame; }               // fn_8005AA34
    void setColor(GXColor color) { mColor = color; }
    void setJointControl(u32 joint, JointControl* control);

    void calcJoints(const Mtx base);                          // fn_800589B0
    void calcView(const Mtx view);                             // fn_80059710

    void drawOpaque();       // fn_80059C54
    void drawTranslucent();  // fn_80059F6C
    void drawShadow();       // fn_8005A280: flat-colored silhouette

    const MdlData* data() const { return mMdl; }
    Mtx& jointMatrix(u32 i) { return mJoint[i].m; }

private:
    struct JointCursor {
        u16 rotKey[3];  // last keyframe index used per rotation axis
        JointControl* control;
    };

    void calcNode(const MdlNode* node, const Mtx parent, const Vec& parentScale, const KeyAnim* anim, f32 frame);
    void sampleJoint(const KeyAnim* anim, u32 joint, Mtx out, f32 frame);
    void calcEnvelopes();  // fn_80058850

    bool nodeHidden(const MdlNode* node) const;
    template <typename Visit>
    void walk(const MdlNode* node, Visit&& visit);

    void drawNodeOpaque(u32 node);      // fn_800597C0
    void drawNodeTranslucent(u32 node); // fn_800598E0
    void drawNodeShadow(u32 node);      // fn_800599E8
    u8 materialAlphaMode(u32 material) const;  // fn_80058B90
    void setupMaterial(u32 material);    // fn_80058C30
    void setupVtxDesc(const MdlShape& shape);  // fn_80058A60
    void drawShape(u32 shape);           // fn_800595B8
    void drawPacket(const MdlPacket& packet);  // fn_80059468
    void setupArrays();

public:
    s32 mPassMode = 0;  // 0x00: shadow pass z-compare mode
private:
    const MdlData* mMdl = nullptr;      // 0x04
    const KeyAnim* mAnim = nullptr;     // 0x08
    f32 mFrame = 0.0f;                  // 0x0C
    f32 mBlend = 0.0f;                  // 0x10
    std::vector<MtxBox> mJoint;            // 0x14 model-space joint (+ envelope) matrices
    std::vector<MtxBox> mView;             // 0x18 view-space position matrices
    std::vector<MtxBox> mNormal;           // 0x1C view-space normal matrices
    std::vector<MtxBox> mPrev;             // 0x20
    GXColor mColor = {0xFF, 0xFF, 0xFF, 0xFF};  // 0x24
    std::vector<JointCursor> mCursor;   // 0x28
};

// --- Global model render state (retail lbl_803CFAB0 / lbl_804D8080 / lbl_804D86A4) ---
void beginModelDraw();             // fn_8005A550
void setMirrorPass(bool mirrored); // lbl_804D8080: selects the mirrored cull-mode row
void setModelTevEnabled(bool on);  // fn_8005C2B8
void setModelZMode(bool compare, bool update);

}  // namespace lm::gfx
