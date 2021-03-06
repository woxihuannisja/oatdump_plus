/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "context_x86.h"

#include "mirror/art_method-inl.h"
#include "quick/quick_method_frame_info.h"
#include "utils.h"


namespace art {
namespace x86 {

static constexpr uintptr_t gZero = 0;

void X86Context::Reset() {
  for (size_t  i = 0; i < kNumberOfCpuRegisters; i++) {
    gprs_[i] = nullptr;
  }
  for (size_t i = 0; i < kNumberOfFloatRegisters; ++i) {
    fprs_[i] = nullptr;
  }
  gprs_[ESP] = &esp_;
  // Initialize registers with easy to spot debug values.
  esp_ = X86Context::kBadGprBase + ESP;
  eip_ = X86Context::kBadGprBase + kNumberOfCpuRegisters;
}

void X86Context::FillCalleeSaves(const StackVisitor& fr) {
  mirror::ArtMethod* method = fr.GetMethod();
  const QuickMethodFrameInfo frame_info = method->GetQuickFrameInfo();
  size_t spill_count = POPCOUNT(frame_info.CoreSpillMask());
  size_t fp_spill_count = POPCOUNT(frame_info.FpSpillMask());
  if (spill_count > 0) {
    // Lowest number spill is farthest away, walk registers and fill into context.
    int j = 2;  // Offset j to skip return address spill.
    for (int i = 0; i < kNumberOfCpuRegisters; i++) {
      if (((frame_info.CoreSpillMask() >> i) & 1) != 0) {
        gprs_[i] = fr.CalleeSaveAddress(spill_count - j, frame_info.FrameSizeInBytes());
        j++;
      }
    }
  }
  if (fp_spill_count > 0) {
    // Lowest number spill is farthest away, walk registers and fill into context.
    size_t j = 2;  // Offset j to skip return address spill.
    size_t fp_spill_size_in_words = fp_spill_count * 2;
    for (size_t i = 0; i < kNumberOfFloatRegisters; ++i) {
      if (((frame_info.FpSpillMask() >> i) & 1) != 0) {
        // There are 2 pieces to each XMM register, to match VR size.
        fprs_[2*i] = reinterpret_cast<uint32_t*>(
            fr.CalleeSaveAddress(spill_count + fp_spill_size_in_words - j,
                                 frame_info.FrameSizeInBytes()));
        fprs_[2*i+1] = reinterpret_cast<uint32_t*>(
            fr.CalleeSaveAddress(spill_count + fp_spill_size_in_words - j - 1,
                                 frame_info.FrameSizeInBytes()));
        // Two void* per XMM register.
        j += 2;
      }
    }
  }
}

void X86Context::SmashCallerSaves() {
  // This needs to be 0 because we want a null/zero return value.
  gprs_[EAX] = const_cast<uintptr_t*>(&gZero);
  gprs_[EDX] = const_cast<uintptr_t*>(&gZero);
  gprs_[ECX] = nullptr;
  gprs_[EBX] = nullptr;
  memset(&fprs_[0], '\0', sizeof(fprs_));
}

void X86Context::SetGPR(uint32_t reg, uintptr_t value) {
  CHECK_LT(reg, static_cast<uint32_t>(kNumberOfCpuRegisters));
  DCHECK(IsAccessibleGPR(reg));
  CHECK_NE(gprs_[reg], &gZero);
  *gprs_[reg] = value;
}

void X86Context::SetFPR(uint32_t reg, uintptr_t value) {
  CHECK_LT(reg, static_cast<uint32_t>(kNumberOfFloatRegisters));
  DCHECK(IsAccessibleFPR(reg));
  CHECK_NE(fprs_[reg], reinterpret_cast<const uint32_t*>(&gZero));
  *fprs_[reg] = value;
}

void X86Context::DoLongJump() {
#if defined(__i386__)
  // Array of GPR values, filled from the context backward for the long jump pop. We add a slot at
  // the top for the stack pointer that doesn't get popped in a pop-all.
  volatile uintptr_t gprs[kNumberOfCpuRegisters + 1];
  for (size_t i = 0; i < kNumberOfCpuRegisters; ++i) {
    gprs[kNumberOfCpuRegisters - i - 1] = gprs_[i] != nullptr ? *gprs_[i] : X86Context::kBadGprBase + i;
  }
  uint32_t fprs[kNumberOfFloatRegisters];
  for (size_t i = 0; i < kNumberOfFloatRegisters; ++i) {
    fprs[i] = fprs_[i] != nullptr ? *fprs_[i] : X86Context::kBadFprBase + i;
  }
  // We want to load the stack pointer one slot below so that the ret will pop eip.
  uintptr_t esp = gprs[kNumberOfCpuRegisters - ESP - 1] - sizeof(intptr_t);
  gprs[kNumberOfCpuRegisters] = esp;
  *(reinterpret_cast<uintptr_t*>(esp)) = eip_;
  __asm__ __volatile__(
      "movl %1, %%ebx\n\t"          // Address base of FPRs.
      "movsd 0(%%ebx), %%xmm0\n\t"  // Load up XMM0-XMM7.
      "movsd 8(%%ebx), %%xmm1\n\t"
      "movsd 16(%%ebx), %%xmm2\n\t"
      "movsd 24(%%ebx), %%xmm3\n\t"
      "movsd 32(%%ebx), %%xmm4\n\t"
      "movsd 40(%%ebx), %%xmm5\n\t"
      "movsd 48(%%ebx), %%xmm6\n\t"
      "movsd 56(%%ebx), %%xmm7\n\t"
      "movl %0, %%esp\n\t"  // ESP points to gprs.
      "popal\n\t"           // Load all registers except ESP and EIP with values in gprs.
      "popl %%esp\n\t"      // Load stack pointer.
      "ret\n\t"             // From higher in the stack pop eip.
      :  // output.
      : "g"(&gprs[0]), "g"(&fprs[0]) // input.
      :);  // clobber.
#else
  UNIMPLEMENTED(FATAL);
#endif
  UNREACHABLE();
}

}  // namespace x86
}  // namespace art
