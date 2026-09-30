# chromatic API Documentation

chromatic is a universal modifier for Chromium/V8 that provides rich low-level memory operations, function interception, breakpoint debugging, and more.

## Table of Contents

- [Process API](#process-api) - Process information
- [Module API](#module-api) - Module operations
- [Memory API](#memory-api) - Memory operations
- [NativePointer API](#nativepointer-api) - Pointer operations
- [NativeFunction API](#nativefunction-api) - Native function calls
- [NativeCallback API](#nativecallback-api) - Native callbacks
- [Interceptor API](#interceptor-api) - Function interception
- [Instruction API](#instruction-api) - Instruction analysis
- [SoftwareBreakpoint API](#softwarebreakpoint-api) - Software breakpoints
- [HardwareBreakpoint API](#hardwarebreakpoint-api) - Hardware breakpoints
- [MemoryAccessMonitor API](#memoryaccessmonitor-api) - Memory access monitoring
- [ExceptionHandler API](#exceptionhandler-api) - Exception handling
- [Utility Functions](#utility-functions)

---

## Process API

Provides process-related information and operations.

### Properties

#### `Process.arch`
Returns the current architecture type.
- **Returns**: `'arm64'` or `'x64'`

```javascript
const arch = Process.arch;
console.log(arch); // 'arm64' or 'x64'
```

#### `Process.platform`
Returns the current platform type.
- **Returns**: `'windows'`, `'linux'`, `'darwin'`, or `'android'`

```javascript
const platform = Process.platform;
console.log(platform); // 'darwin'
```

#### `Process.pointerSize`
Returns the pointer size in bytes.
- **Returns**: `4` (32-bit) or `8` (64-bit)

```javascript
const size = Process.pointerSize;
console.log(size); // 8
```

#### `Process.pageSize`
Returns the system page size.
- **Returns**: Page size in bytes, typically 4096 or larger

```javascript
const pageSize = Process.pageSize;
console.log(pageSize); // 4096
```

### Methods

#### `Process.enumerateModules()`
Enumerates all loaded modules in the process.
- **Returns**: `Module[]` - Array of module information

```javascript
const modules = Process.enumerateModules();
modules.forEach(m => {
  console.log(`${m.name}: ${m.base} (${m.size} bytes)`);
});
```

Each module object contains:
- `name`: Module name
- `base`: Module base address (NativePointer)
- `size`: Module size in bytes
- `path`: Module file path

#### `Process.enumerateRanges(protection)`
Enumerates memory ranges with specified protection attributes.
- **Parameters**:
  - `protection`: Protection string like `'r--'`, `'rw-'`, `'r-x'`, etc.
- **Returns**: `Range[]` - Array of memory ranges

```javascript
const ranges = Process.enumerateRanges('r--');
ranges.forEach(r => {
  console.log(`${r.base} - ${r.protection}`);
});
```

Each range object contains:
- `base`: Range base address (NativePointer)
- `size`: Range size
- `protection`: Protection string
- `filePath`: Associated file path (if any)

#### `Process.findModuleByAddress(address)`
Finds the module containing the specified address.
- **Parameters**:
  - `address`: Address (NativePointer)
- **Returns**: Module object or `null`

```javascript
const addr = Module.findExportByName(null, 'malloc');
const mod = Process.findModuleByAddress(addr);
console.log(mod.name); // 'libc.so.6' or similar
```

---

## Module API

Provides module lookup and export operations.

### Methods

#### `Module.findExportByName(moduleName, exportName)`
Finds the address of an exported function in the specified module.
- **Parameters**:
  - `moduleName`: Module name (`null` to search all modules)
  - `exportName`: Export function name
- **Returns**: NativePointer or `null`

```javascript
const malloc = Module.findExportByName(null, 'malloc');
console.log(malloc); // NativePointer object
```

#### `Module.enumerateExports(moduleName)`
Enumerates all exports of the specified module.
- **Parameters**:
  - `moduleName`: Module name
- **Returns**: Array of export information

```javascript
const exports = Module.enumerateExports('libc.so.6');
exports.forEach(e => {
  console.log(`${e.type} ${e.name}: ${e.address}`);
});
```

Each export object contains:
- `type`: Export type (`'function'` or `'variable'`)
- `name`: Export name
- `address`: Export address (NativePointer)

#### `Module.load(moduleName)`
Loads the specified module.
- **Parameters**:
  - `moduleName`: Module name or path
- **Returns**: Module object

```javascript
const mod = Module.load('mylib.so');
console.log(mod.base);
```

---

## Memory API

Provides memory allocation, read/write, scanning, and other operations.

### Methods

#### `Memory.alloc(size)`
Allocates executable memory.
- **Parameters**:
  - `size`: Allocation size in bytes
- **Returns**: NativePointer - Allocated memory address

```javascript
const buf = Memory.alloc(64);
buf.writeU32(0xCAFEBABE);
```

#### `Memory.copy(dst, src, size)`
Copies memory.
- **Parameters**:
  - `dst`: Destination address (NativePointer)
  - `src`: Source address (NativePointer)
  - `size`: Copy size

```javascript
const src = Memory.alloc(16);
src.writeU32(0x12345678);
const dst = Memory.alloc(16);
Memory.copy(dst, src, 4);
```

#### `Memory.protect(address, size, protection)`
Modifies memory protection attributes.
- **Parameters**:
  - `address`: Memory address (NativePointer)
  - `size`: Size
  - `protection`: Protection string like `'r--'`, `'rw-'`, `'rwx'`, etc.
- **Returns**: Old protection string

```javascript
const p = Memory.alloc(4096);
Memory.protect(p, 4096, 'rwx');
```

#### `Memory.scanSync(address, size, pattern)`
Synchronously scans for a memory pattern.
- **Parameters**:
  - `address`: Start address (NativePointer)
  - `size`: Scan range size
  - `pattern`: Hex pattern string, supports wildcard `??`
- **Returns**: Array of match results

```javascript
const results = Memory.scanSync(buf, 64, 'ef be ad de');
results.forEach(r => {
  console.log(`Found at ${r.address}`);
});
```

#### `Memory.scan(address, size, pattern)`
Asynchronously scans for a memory pattern.
- **Parameters**: Same as `scanSync`
- **Returns**: Promise<Array of match results>

```javascript
const results = await Memory.scan(buf, 64, 'ef be ad de');
```

#### `Memory.scanModule(moduleName, pattern)`
Scans for a memory pattern within a module.
- **Parameters**:
  - `moduleName`: Module name
  - `pattern`: Hex pattern string
- **Returns**: Array of match results

```javascript
const results = Memory.scanModule('libc.so.6', '48 8b ?? 00');
```

---

## NativePointer API

Provides pointer operation functionality.

### Constructor

```javascript
const p1 = new NativePointer(0x1234);
const p2 = ptr('0xdeadbeef');
const p3 = ptr(0);
```

### Methods

#### `isNull()`
Checks if the pointer is null.
- **Returns**: boolean

```javascript
if (ptr(0).isNull()) {
  console.log('Null pointer');
}
```

#### `add(offset)` / `sub(offset)`
Pointer arithmetic operations.
- **Parameters**:
  - `offset`: Offset value
- **Returns**: New NativePointer

```javascript
const p = ptr(100).add(50); // 150
const q = ptr(200).sub(50); // 150
```

#### `and(ptr)` / `or(ptr)` / `xor(ptr)`
Bitwise operations.
- **Parameters**:
  - `ptr`: Another pointer or number
- **Returns**: New NativePointer

```javascript
const a = ptr(0xFF00);
const b = ptr(0x0FF0);
const result = a.and(b); // 0x0F00
```

#### `compare(ptr)`
Compares two pointers.
- **Returns**: `-1` (less than), `0` (equal), `1` (greater than)

```javascript
const a = ptr(100);
const b = ptr(200);
console.log(a.compare(b)); // -1
```

#### `equals(ptr)`
Checks if two pointers are equal.
- **Returns**: boolean

```javascript
if (ptr(100).equals(ptr(100))) {
  console.log('Equal');
}
```

#### `toString()`
Converts to hexadecimal string.
- **Returns**: string

```javascript
console.log(ptr(0x1234).toString()); // '0x1234'
```

#### `toUInt32()`
Converts to unsigned 32-bit integer.
- **Returns**: number

```javascript
const val = ptr('0xdeadbeef').toUInt32();
console.log(val); // 3735928559
```

#### Read/Write Methods

```javascript
const p = Memory.alloc(64);

// Write
p.writeU8(0xFF);
p.writeU16(0x1234);
p.writeU32(0xDEADBEEF);
p.writeU64(0x123456789ABCDEF0n);

// Read
const u8 = p.readU8();
const u16 = p.readU16();
const u32 = p.readU32();
const u64 = p.readU64();
```

---

## NativeFunction API

Calls native functions.

### Constructor

```javascript
const fn = new NativeFunction(address, returnType, argTypes, options?);
```

- **Parameters**:
  - `address`: Function address (NativePointer)
  - `returnType`: Return type string
  - `argTypes`: Array of argument types
  - `options`: Optional configuration (e.g., ABI)

**Supported Types**:
- `'void'`
- `'int'`, `'uint'`
- `'long'`, `'ulong'`
- `'int8'`, `'uint8'`, `'int16'`, `'uint16'`, `'int32'`, `'uint32'`, `'int64'`, `'uint64'`
- `'float'`, `'double'`
- `'pointer'`

**Supported ABIs**:
- `'default'`
- `'sysv'` (System V AMD64 ABI)
- `'stdcall'` (Windows)
- `'win64'`

### Examples

```javascript
const malloc = Module.findExportByName(null, 'malloc');
const fn = new NativeFunction(malloc, 'pointer', ['size_t']);
const buf = fn(1024);
```

```javascript
const add = new NativeFunction(addr, 'int', ['int', 'int']);
const result = add(3, 4); // 7
```

---

## NativeCallback API

Creates native callback functions.

### Constructor

```javascript
const cb = new NativeCallback(func, returnType, argTypes);
```

- **Parameters**:
  - `func`: JavaScript callback function
  - `returnType`: Return type
  - `argTypes`: Array of argument types

### Properties

#### `cb.address`
The address of the callback function (NativePointer).

### Methods

#### `cb.destroy()`
Destroys the callback and releases resources.

### Example

```javascript
const cb = new NativeCallback(function(a, b) {
  return a + b;
}, 'int', ['int', 'int']);

const fn = new NativeFunction(cb.address, 'int', ['int', 'int']);
const result = fn(10, 20); // 30

cb.destroy();
```

---

## Interceptor API

Intercepts function calls.

### Methods

#### `Interceptor.attach(target, callbacks)`
Attaches an interceptor to the target function.
- **Parameters**:
  - `target`: Target function address (NativePointer)
  - `callbacks`: Callback object containing `onEnter` and/or `onLeave`
- **Returns**: Interceptor listener object

```javascript
const target = Module.findExportByName(null, 'malloc');
const listener = Interceptor.attach(target, {
  onEnter(args) {
    console.log('malloc called with size:', args[0]);
  },
  onLeave(retval) {
    console.log('malloc returned:', retval);
  }
});
```

Parameters received by callback functions:
- `onEnter(args)`:
  - `args`: Array of arguments, each is a NativePointer
  - `this.context`: CPU context (NativePointer)
  - `this.returnAddress`: Return address

- `onLeave(retval)`:
  - `retval`: the return value (NativePointer); call `retval.replace(ptr(...))` to change it
  - `this.context`: the CPU context
  - `this.returnValue`: the same object as `retval`; replacing through either name works

> **Note**: `onLeave` runs after the original function has returned - the engine calls
> the function in place, takes the result, then invokes the callback. One limitation
> follows from that: a function taking arguments **on the stack** (more than eight
> integer arguments, or a large struct passed by value) will read them from the wrong
> place, because the engine's frame sits between the caller and the function. Hooks
> that use only `onEnter` are unaffected.

#### `listener.detach()`
Detaches the interceptor.

```javascript
listener.detach();
```

#### `Interceptor.detachAll()`
Detaches all interceptors.

```javascript
Interceptor.detachAll();
```

---

## Instruction API

Instruction analysis and disassembly.

### Methods

#### `Instruction.parse(address)`
Parses a single instruction.
- **Parameters**:
  - `address`: Instruction address (NativePointer)
- **Returns**: Instruction information object

```javascript
const addr = Module.findExportByName(null, 'malloc');
const insn = Instruction.parse(addr);
console.log(insn.mnemonic, insn.opStr);
```

Instruction object contains:
- `mnemonic`: Mnemonic
- `opStr`: Operand string
- `size`: Instruction size
- `address`: Instruction address (NativePointer)
- `bytes`: Instruction bytes (hex string)

#### `Instruction.disassemble(address, count)`
Disassembles multiple instructions.
- **Parameters**:
  - `address`: Start address (NativePointer)
  - `count`: Number of instructions
- **Returns**: Array of instruction information

```javascript
const insns = Instruction.disassemble(addr, 5);
insns.forEach(insn => {
  console.log(`${insn.address}: ${insn.mnemonic} ${insn.opStr}`);
});
```

#### `Instruction.analyze(address)`
Analyzes instruction control flow characteristics.
- **Parameters**:
  - `address`: Instruction address (NativePointer)
- **Returns**: Analysis result object

```javascript
const analysis = Instruction.analyze(addr);
console.log('Is branch:', analysis.isBranch);
console.log('Is call:', analysis.isCall);
```

Analysis object contains:
- `isBranch`: Whether it's a branch instruction
- `isCall`: Whether it's a call instruction
- `isRelative`: Whether it's relative addressing
- `target`: Target address (hex string)
- `isPcRelative`: Whether it's PC-relative
- `size`: Instruction size

#### `Instruction.filterInstructions(address, count, filter)`
Filters instructions.
- **Parameters**:
  - `address`: Start address (NativePointer)
  - `count`: Number of instructions
  - `filter`: Filter function
- **Returns**: Array of instruction information

```javascript
const calls = Instruction.filterInstructions(addr, 100, (insn) => {
  return insn.mnemonic === 'call';
});
```

#### `Instruction.filterInstructionsAsync(address, count, filter)`
Asynchronously filters instructions.
- **Parameters**: Same as `filterInstructions`
- **Returns**: Promise<Array of instruction information>

```javascript
const results = await Instruction.filterInstructionsAsync(addr, 100, filter);
```

#### `Instruction.findXrefs(rangeStart, rangeSize, targetAddr)`
Finds cross-references.
- **Parameters**:
  - `rangeStart`: Range start address (NativePointer)
  - `rangeSize`: Range size
  - `targetAddr`: Target address (NativePointer)
- **Returns**: Array of cross-reference results

```javascript
const xrefs = Instruction.findXrefs(addr, 256, target);
xrefs.forEach(xref => {
  console.log(`${xref.address}: ${xref.type}`);
});
```

Each cross-reference object contains:
- `address`: Reference address (hex string)
- `type`: Reference type (`'call'`, `'branch'`, or `'data'`)
- `size`: Instruction size

---

## SoftwareBreakpoint API

Software breakpoints (INT3 / BRK).

### Methods

#### `SoftwareBreakpoint.set(address, callback)`
Sets a software breakpoint.
- **Parameters**:
  - `address`: Breakpoint address (NativePointer)
  - `callback`: Callback function when triggered
- **Returns**: Breakpoint object

```javascript
const target = Module.findExportByName(null, 'malloc');
const bp = SoftwareBreakpoint.set(target, () => {
  console.log('Breakpoint hit!');
});
```

#### `bp.remove()`
Removes the breakpoint.

```javascript
bp.remove();
```

#### `SoftwareBreakpoint.removeAll()`
Removes all software breakpoints.

```javascript
SoftwareBreakpoint.removeAll();
```

---

## HardwareBreakpoint API

Hardware breakpoints (debug registers).

### Properties

#### `HardwareBreakpoint.maxBreakpoints`
Maximum number of hardware breakpoints.
- **Returns**: number (typically 4, or 0 if not supported)

```javascript
const max = HardwareBreakpoint.maxBreakpoints;
console.log('Max hardware breakpoints:', max);
```

#### `HardwareBreakpoint.activeCount`
Current number of active hardware breakpoints.
- **Returns**: number

```javascript
const count = HardwareBreakpoint.activeCount;
```

### Methods

#### `HardwareBreakpoint.set(address, type, size, callback)`
Sets a hardware breakpoint/watchpoint.
- **Parameters**:
  - `address`: Address (NativePointer)
  - `type`: Type (`'execute'`, `'write'`, or `'readwrite'`)
  - `size`: Size (1, 2, 4, or 8 bytes; ignored for execute)
  - `callback`: Callback function when triggered
- **Returns**: Breakpoint object

```javascript
// Execute breakpoint
const bp1 = HardwareBreakpoint.set(addr, 'execute', 1, () => {
  console.log('Execute breakpoint hit!');
});

// Write watchpoint
const buf = Memory.alloc(8);
const bp2 = HardwareBreakpoint.set(buf, 'write', 4, () => {
  console.log('Write watchpoint hit!');
});
```

#### `bp.remove()`
Removes the hardware breakpoint.

```javascript
bp.remove();
```

#### `HardwareBreakpoint.removeAll()`
Removes all hardware breakpoints.

```javascript
HardwareBreakpoint.removeAll();
```

---

## MemoryAccessMonitor API

Memory access monitoring (based on page protection).

### Methods

#### `MemoryAccessMonitor.enable(ranges, callback)`
Enables memory access monitoring.
- **Parameters**:
  - `ranges`: Array of ranges, each containing `address` and `size`
  - `callback`: Callback function on access
- **Returns**: Monitor object

```javascript
const buf = Memory.alloc(4096);
const handle = MemoryAccessMonitor.enable(
  [{ address: buf, size: 4096 }],
  (details) => {
    console.log('Access at:', details.address);
    console.log('Operation:', details.operation);
    console.log('Range index:', details.rangeIndex);
  }
);
```

The `details` object received by the callback contains:
- `address`: Access address (NativePointer)
- `pageBase`: Page base address (NativePointer)
- `operation`: Operation type (`'read'`, `'write'`, or `'execute'`)
- `rangeIndex`: Triggered range index

#### `handle.disable()`
Disables the monitor.

```javascript
handle.disable();
```

#### `MemoryAccessMonitor.disableAll()`
Disables all monitors.

```javascript
MemoryAccessMonitor.disableAll();
```

#### `MemoryAccessMonitor.drain()`
Processes pending access events.
- **Returns**: Number of processed events

```javascript
const count = MemoryAccessMonitor.drain();
console.log('Drained', count, 'events');
```

---

## ExceptionHandler API

Exception handler management.

### Properties

#### `ExceptionHandler.isEnabled`
Checks if the exception handler is enabled.
- **Returns**: boolean

```javascript
if (ExceptionHandler.isEnabled) {
  console.log('Exception handler is enabled');
}
```

### Methods

#### `ExceptionHandler.enable()`
Enables the global exception handler.

```javascript
ExceptionHandler.enable();
```

#### `ExceptionHandler.disable()`
Disables the global exception handler.

```javascript
ExceptionHandler.disable();
```

---

## Utility Functions

### `ptr(value)`
Shortcut for creating a NativePointer.

```javascript
const p = ptr(0x1234);
const q = ptr('0xdeadbeef');
```

### `hexdump(address, options)`
Generates a hexadecimal dump.
- **Parameters**:
  - `address`: Start address (NativePointer)
  - `options`: Configuration object
    - `length`: Dump length
- **Returns**: Hexadecimal dump string

```javascript
const buf = Memory.alloc(32);
const dump = hexdump(buf, { length: 32 });
console.log(dump);
```
