#ifndef MR_ARRAY_HPP
#define MR_ARRAY_HPP

// Based on some eerily similar findings from SMG: https://github.com/SMGCommunity/Petari/blob/master/include/Game/Util/Array.hpp

#include <types.h>

namespace MR {
    template <typename T, s32 S>
    class AssignableArray {
    public:
        AssignableArray() { mArraySize = 0; }
        ~AssignableArray() {}

        s32 getSize() const { return mArraySize; }
        void resetSize() { mArraySize = 0; }

        T& operator[](int idx) { return mArr[idx]; }
        const T& operator[](int idx) const { return mArr[idx]; }

        T* getArrayMember(s32 idx) const { return &mArr[idx]; }
        T* getArray() { return mArr; }
        
        T* getFirstMember() { return &mArr[0]; }
        T* getLastMember() { return &mArr[mArraySize]; }
        T* getMaxIter() { return &mArr[mArraySize - 1]; }
        
        void init(s32 num) {
            mArr = new T[num];
            mArraySize = num;
        }

        void addMember(T* member) {
            mArr[mArraySize++] = *member;
        }

        T* eraseMember(T* pIter) {
            if (getLastMember() - pIter - 1 > 0) {
                for (T* curr = pIter; (curr + 1) != getLastMember(); curr++) {
                    *curr = *(curr + 1);
                } 
            }
            
            mArraySize--;
            return pIter;
        }

    private:
        /* 0x00 */ T mArr[S];
        /* (0x4 * S) + 4 */ s32 mArraySize;
    };

    template<typename T, s32 N>
    class FixedArray {
        public:
            T& operator[](int idx) { return mArr[idx]; }
            const T& operator[](int idx) const { return mArr[idx]; }

            s32 size() const { return N; }
            T* begin() { return &mArr[0]; }
            T* end() { return &mArr[N]; }

        private:
            /* 0x00 */ T mArr[N];
    };
};

#endif
