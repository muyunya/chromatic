import { NativeInterceptor, NativeProcess, NativePointer } from 'chromatic';
import { ptr } from '../native-pointer';
import type {
  NativePointerValue,
  InvocationCallbacks,
  InvocationListener,
  InvocationContext,
  InvocationArgs,
  InvocationReturnValue,
} from '../types';

const isArm64 = NativeProcess.architecture === 'arm64';

/**
 * Where each integer argument lives in the x86_64 register snapshot the trampoline
 * pushes.
 *
 * The snapshot is pushed rax, rcx, rdx, rbx, rbp, rsi, rdi, r8 ... r15, so counting
 * from its base the slots run r15 = 0, r14 = 1, r13 = 2, r12 = 3, r11 = 4, r10 = 5,
 * r9 = 6, r8 = 7, rdi = 8, rsi = 9, rbp = 10, rbx = 11, rdx = 12, rcx = 13, rax = 14.
 * The engine itself pins that layout down: the leave dispatcher reads the return value
 * from slot 14, which is where rax lands under this reading and nowhere else.
 *
 * The table this replaces said `[7, 6, 3, 2, 8, 9]`, which is every entry one slot too
 * low and calls r8 the first argument on both ABIs. On x86_64 `args` read the wrong
 * registers entirely - r8 where rdi belongs - and so did writes through it.
 */
function x86ArgumentSlot(index: number): number {
  // System V: rdi, rsi, rdx, rcx, r8, r9. Microsoft x64: rcx, rdx, r8, r9.
  const slots =
    NativeProcess.platform === 'windows' ? [13, 12, 7, 6] : [8, 9, 12, 13, 7, 6];
  return index < slots.length ? slots[index] : -1;
}

/**
 * Interceptor — Frida-compatible inline hook API.
 * Now a thin wrapper around C++ NativeInterceptor (trampoline + relocator done in C++).
 */
export const Interceptor = {
  /**
   * Attach inline hook to `target`.
   */
  attach(target: NativePointerValue, callbacks: InvocationCallbacks): InvocationListener {
    const p = ptr(target);

    const onEnter = (cpuContextPtr: string) => {
      if (!callbacks.onEnter) return;

      const ctx: InvocationContext = {
        context: {} as any,
        threadId: 0,
        returnAddress: new NativePointer(0),
      };

      // Build args proxy
      const argsProxy = new Proxy({} as InvocationArgs, {
        get(_target, prop) {
          if (typeof prop === 'string') {
            const idx = parseInt(prop, 10);
            if (!isNaN(idx)) {
              const ctxPtr = new NativePointer(parseInt(cpuContextPtr, 16));
              const ptrSize = NativeProcess.pointerSize;
              if (isArm64) {
                return ctxPtr.add(idx * ptrSize).readPointer();
              } else {
                const slot = x86ArgumentSlot(idx);
                if (slot >= 0) {
                  return ctxPtr.add(slot * ptrSize).readPointer();
                }
                return new NativePointer(0);
              }
            }
          }
          return undefined;
        },
        set(_target, prop, value) {
          if (typeof prop === 'string') {
            const idx = parseInt(prop, 10);
            if (!isNaN(idx)) {
              const ctxPtr = new NativePointer(parseInt(cpuContextPtr, 16));
              const ptrSize = NativeProcess.pointerSize;
              if (isArm64) {
                ctxPtr.add(idx * ptrSize).writePointer(ptr(value));
              } else {
                const slot = x86ArgumentSlot(idx);
                if (slot >= 0) {
                  ctxPtr.add(slot * ptrSize).writePointer(ptr(value));
                }
              }
              return true;
            }
          }
          return false;
        }
      });

      try {
        callbacks.onEnter!.call(ctx, argsProxy);
      } catch (e) {
        // Swallow errors in hook callbacks to avoid crashing the target
      }
    };

    const onLeave = (cpuContextPtr: string) => {
      if (!callbacks.onLeave) return;

      const ctxPtr = new NativePointer(parseInt(cpuContextPtr, 16));
      const ptrSize = NativeProcess.pointerSize;

      const retvalPtr = isArm64 ? ctxPtr : ctxPtr.add(14 * ptrSize);
      const retval = retvalPtr.readPointer() as any as InvocationReturnValue;
      retval.replace = (value: NativePointerValue) => {
        retvalPtr.writePointer(ptr(value));
      };

      const ctx: InvocationContext = {
        context: {} as any,
        threadId: 0,
        returnAddress: new NativePointer(0),
        returnValue: retval,
      };

      try {
        callbacks.onLeave!.call(ctx, retval);
      } catch (e) {
        // Swallow
      }
    };

    const hookId = NativeInterceptor.attach(p, onEnter, onLeave);

    return {
      detach() {
        NativeInterceptor.detach(hookId);
      }
    };
  },

  /**
   * Replace target function with `replacement`. Returns a NativePointer
   * to a trampoline that calls the original function.
   */
  replace(target: NativePointerValue, replacement: NativePointerValue): NativePointer {
    return NativeInterceptor.replace(ptr(target), ptr(replacement));
  },

  /**
   * Revert a hook/replace at `target`.
   */
  revert(target: NativePointerValue): void {
    NativeInterceptor.revert(ptr(target));
  },

  /**
   * Detach all active hooks.
   */
  detachAll(): void {
    NativeInterceptor.detachAll();
  },
};
