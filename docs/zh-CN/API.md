# chromatic API 文档

chromatic 是一个通用的 Chromium/V8 修改器，提供了丰富的底层内存操作、函数拦截、断点调试等功能。

## 目录

- [Process API](#process-api) - 进程信息
- [Module API](#module-api) - 模块操作
- [Memory API](#memory-api) - 内存操作
- [NativePointer API](#nativepointer-api) - 指针操作
- [NativeFunction API](#nativefunction-api) - 原生函数调用
- [NativeCallback API](#nativecallback-api) - 原生回调
- [Interceptor API](#interceptor-api) - 函数拦截
- [Instruction API](#instruction-api) - 指令分析
- [SoftwareBreakpoint API](#softwarebreakpoint-api) - 软件断点
- [HardwareBreakpoint API](#hardwarebreakpoint-api) - 硬件断点
- [MemoryAccessMonitor API](#memoryaccessmonitor-api) - 内存访问监控
- [ExceptionHandler API](#exceptionhandler-api) - 异常处理
- [工具函数](#工具函数)

---

## Process API

提供进程相关的信息和操作。

### 属性

#### `Process.arch`
返回当前架构类型。
- **返回值**: `'arm64'` 或 `'x64'`

```javascript
const arch = Process.arch;
console.log(arch); // 'arm64' 或 'x64'
```

#### `Process.platform`
返回当前平台类型。
- **返回值**: `'windows'`、`'linux'`、`'darwin'` 或 `'android'`

```javascript
const platform = Process.platform;
console.log(platform); // 'darwin'
```

#### `Process.pointerSize`
返回指针大小（字节）。
- **返回值**: `4`（32位）或 `8`（64位）

```javascript
const size = Process.pointerSize;
console.log(size); // 8
```

#### `Process.pageSize`
返回系统页面大小。
- **返回值**: 页面大小（字节），通常为 4096 或更大

```javascript
const pageSize = Process.pageSize;
console.log(pageSize); // 4096
```

### 方法

#### `Process.enumerateModules()`
枚举进程中加载的所有模块。
- **返回值**: `Module[]` - 模块信息数组

```javascript
const modules = Process.enumerateModules();
modules.forEach(m => {
  console.log(`${m.name}: ${m.base} (${m.size} bytes)`);
});
```

每个模块对象包含：
- `name`: 模块名称
- `base`: 模块基址（NativePointer）
- `size`: 模块大小（字节）
- `path`: 模块文件路径

#### `Process.enumerateRanges(protection)`
枚举具有指定保护属性的内存范围。
- **参数**:
  - `protection`: 保护属性字符串，如 `'r--'`、`'rw-'`、`'r-x'` 等
- **返回值**: `Range[]` - 内存范围数组

```javascript
const ranges = Process.enumerateRanges('r--');
ranges.forEach(r => {
  console.log(`${r.base} - ${r.protection}`);
});
```

每个范围对象包含：
- `base`: 范围基址（NativePointer）
- `size`: 范围大小
- `protection`: 保护属性字符串
- `filePath`: 关联的文件路径（如果有）

#### `Process.findModuleByAddress(address)`
根据地址查找包含该地址的模块。
- **参数**:
  - `address`: 地址（NativePointer）
- **返回值**: 模块对象或 `null`

```javascript
const addr = Module.findExportByName(null, 'malloc');
const mod = Process.findModuleByAddress(addr);
console.log(mod.name); // 'libc.so.6' 或类似
```

---

## Module API

提供模块查找和导出操作。

### 方法

#### `Module.findExportByName(moduleName, exportName)`
查找指定模块中的导出函数地址。
- **参数**:
  - `moduleName`: 模块名称（`null` 表示搜索所有模块）
  - `exportName`: 导出函数名称
- **返回值**: NativePointer 或 `null`

```javascript
const malloc = Module.findExportByName(null, 'malloc');
console.log(malloc); // NativePointer 对象
```

#### `Module.enumerateExports(moduleName)`
枚举指定模块的所有导出符号。
- **参数**:
  - `moduleName`: 模块名称
- **返回值**: 导出信息数组

```javascript
const exports = Module.enumerateExports('libc.so.6');
exports.forEach(e => {
  console.log(`${e.type} ${e.name}: ${e.address}`);
});
```

每个导出对象包含：
- `type`: 导出类型（`'function'` 或 `'variable'`）
- `name`: 导出名称
- `address`: 导出地址（NativePointer）

#### `Module.load(moduleName)`
加载指定模块。
- **参数**:
  - `moduleName`: 模块名称或路径
- **返回值**: 模块对象

```javascript
const mod = Module.load('mylib.so');
console.log(mod.base);
```

---

## Memory API

提供内存分配、读写、扫描等操作。

### 方法

#### `Memory.alloc(size)`
分配可执行内存。
- **参数**:
  - `size`: 分配大小（字节）
- **返回值**: NativePointer - 分配的内存地址

```javascript
const buf = Memory.alloc(64);
buf.writeU32(0xCAFEBABE);
```

#### `Memory.copy(dst, src, size)`
复制内存。
- **参数**:
  - `dst`: 目标地址（NativePointer）
  - `src`: 源地址（NativePointer）
  - `size`: 复制大小

```javascript
const src = Memory.alloc(16);
src.writeU32(0x12345678);
const dst = Memory.alloc(16);
Memory.copy(dst, src, 4);
```

#### `Memory.protect(address, size, protection)`
修改内存保护属性。
- **参数**:
  - `address`: 内存地址（NativePointer）
  - `size`: 大小
  - `protection`: 保护属性字符串，如 `'r--'`、`'rw-'`、`'rwx'` 等
- **返回值**: 旧的保护属性字符串

```javascript
const p = Memory.alloc(4096);
Memory.protect(p, 4096, 'rwx');
```

#### `Memory.scanSync(address, size, pattern)`
同步扫描内存模式。
- **参数**:
  - `address`: 起始地址（NativePointer）
  - `size`: 扫描范围大小
  - `pattern`: 十六进制模式字符串，支持通配符 `??`
- **返回值**: 匹配结果数组

```javascript
const results = Memory.scanSync(buf, 64, 'ef be ad de');
results.forEach(r => {
  console.log(`Found at ${r.address}`);
});
```

#### `Memory.scan(address, size, pattern)`
异步扫描内存模式。
- **参数**: 同 `scanSync`
- **返回值**: Promise<匹配结果数组>

```javascript
const results = await Memory.scan(buf, 64, 'ef be ad de');
```

#### `Memory.scanModule(moduleName, pattern)`
在模块中扫描内存模式。
- **参数**:
  - `moduleName`: 模块名称
  - `pattern`: 十六进制模式字符串
- **返回值**: 匹配结果数组

```javascript
const results = Memory.scanModule('libc.so.6', '48 8b ?? 00');
```

---

## NativePointer API

提供指针操作功能。

### 构造函数

```javascript
const p1 = new NativePointer(0x1234);
const p2 = ptr('0xdeadbeef');
const p3 = ptr(0);
```

### 方法

#### `isNull()`
检查指针是否为空。
- **返回值**: boolean

```javascript
if (ptr(0).isNull()) {
  console.log('Null pointer');
}
```

#### `add(offset)` / `sub(offset)`
指针算术运算。
- **参数**:
  - `offset`: 偏移量
- **返回值**: 新的 NativePointer

```javascript
const p = ptr(100).add(50); // 150
const q = ptr(200).sub(50); // 150
```

#### `and(ptr)` / `or(ptr)` / `xor(ptr)`
位运算。
- **参数**:
  - `ptr`: 另一个指针或数值
- **返回值**: 新的 NativePointer

```javascript
const a = ptr(0xFF00);
const b = ptr(0x0FF0);
const result = a.and(b); // 0x0F00
```

#### `compare(ptr)`
比较两个指针。
- **返回值**: `-1`（小于）、`0`（等于）、`1`（大于）

```javascript
const a = ptr(100);
const b = ptr(200);
console.log(a.compare(b)); // -1
```

#### `equals(ptr)`
检查两个指针是否相等。
- **返回值**: boolean

```javascript
if (ptr(100).equals(ptr(100))) {
  console.log('Equal');
}
```

#### `toString()`
转换为十六进制字符串。
- **返回值**: string

```javascript
console.log(ptr(0x1234).toString()); // '0x1234'
```

#### `toUInt32()`
转换为无符号 32 位整数。
- **返回值**: number

```javascript
const val = ptr('0xdeadbeef').toUInt32();
console.log(val); // 3735928559
```

#### 读写方法

```javascript
const p = Memory.alloc(64);

// 写入
p.writeU8(0xFF);
p.writeU16(0x1234);
p.writeU32(0xDEADBEEF);
p.writeU64(0x123456789ABCDEF0n);

// 读取
const u8 = p.readU8();
const u16 = p.readU16();
const u32 = p.readU32();
const u64 = p.readU64();
```

---

## NativeFunction API

调用原生函数。

### 构造函数

```javascript
const fn = new NativeFunction(address, returnType, argTypes, options?);
```

- **参数**:
  - `address`: 函数地址（NativePointer）
  - `returnType`: 返回类型字符串
  - `argTypes`: 参数类型数组
  - `options`: 可选配置（如 ABI）

**支持的类型**:
- `'void'`
- `'int'`, `'uint'`
- `'long'`, `'ulong'`
- `'int8'`, `'uint8'`, `'int16'`, `'uint16'`, `'int32'`, `'uint32'`, `'int64'`, `'uint64'`
- `'float'`, `'double'`
- `'pointer'`

**支持的 ABI**:
- `'default'`
- `'sysv'`（System V AMD64 ABI）
- `'stdcall'`（Windows）
- `'win64'`

### 示例

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

创建原生回调函数。

### 构造函数

```javascript
const cb = new NativeCallback(func, returnType, argTypes);
```

- **参数**:
  - `func`: JavaScript 回调函数
  - `returnType`: 返回类型
  - `argTypes`: 参数类型数组

### 属性

#### `cb.address`
回调函数的地址（NativePointer）。

### 方法

#### `cb.destroy()`
销毁回调并释放资源。

### 示例

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

拦截函数调用。

### 方法

#### `Interceptor.attach(target, callbacks)`
附加拦截器到目标函数。
- **参数**:
  - `target`: 目标函数地址（NativePointer）
  - `callbacks`: 回调对象，包含 `onEnter` 和/或 `onLeave`
- **返回值**: 拦截器监听器对象

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

回调函数接收的参数：
- `onEnter(args)`:
  - `args`: 参数数组，每个参数都是 NativePointer
  - `this.context`: CPU 上下文（NativePointer）
  - `this.returnAddress`: 返回地址

- `onLeave(retval)`:
  - `retval`: 返回值（NativePointer），可用 `retval.replace(ptr(...))` 改写
  - `this.context`: CPU 上下文
  - `this.returnValue`: 与 `retval` 是同一个对象，用哪个改写都一样

> **注意**：`onLeave` 是在原函数**返回之后**才调用的——引擎在原地调用原函数、拿到返回值，
> 再回调。由此带来一个限制：如果目标函数**通过栈传递参数**（整型参数超过 8 个，或按值传递
> 较大的结构体），这些参数会被读错，因为引擎的一帧位于调用者与函数之间。只使用 `onEnter`
> 的 hook 不受影响。

#### `listener.detach()`
分离拦截器。

```javascript
listener.detach();
```

#### `Interceptor.detachAll()`
分离所有拦截器。

```javascript
Interceptor.detachAll();
```

---

## Instruction API

指令分析和反汇编。

### 方法

#### `Instruction.parse(address)`
解析单个指令。
- **参数**:
  - `address`: 指令地址（NativePointer）
- **返回值**: 指令信息对象

```javascript
const addr = Module.findExportByName(null, 'malloc');
const insn = Instruction.parse(addr);
console.log(insn.mnemonic, insn.opStr);
```

指令对象包含：
- `mnemonic`: 助记符
- `opStr`: 操作数字符串
- `size`: 指令大小
- `address`: 指令地址（NativePointer）
- `bytes`: 指令字节（十六进制字符串）

#### `Instruction.disassemble(address, count)`
反汇编多条指令。
- **参数**:
  - `address`: 起始地址（NativePointer）
  - `count`: 指令数量
- **返回值**: 指令信息数组

```javascript
const insns = Instruction.disassemble(addr, 5);
insns.forEach(insn => {
  console.log(`${insn.address}: ${insn.mnemonic} ${insn.opStr}`);
});
```

#### `Instruction.analyze(address)`
分析指令的控制流特性。
- **参数**:
  - `address`: 指令地址（NativePointer）
- **返回值**: 分析结果对象

```javascript
const analysis = Instruction.analyze(addr);
console.log('Is branch:', analysis.isBranch);
console.log('Is call:', analysis.isCall);
```

分析对象包含：
- `isBranch`: 是否为分支指令
- `isCall`: 是否为调用指令
- `isRelative`: 是否为相对地址
- `target`: 目标地址（十六进制字符串）
- `isPcRelative`: 是否为 PC 相对
- `size`: 指令大小

#### `Instruction.filterInstructions(address, count, filter)`
过滤指令。
- **参数**:
  - `address`: 起始地址（NativePointer）
  - `count`: 指令数量
  - `filter`: 过滤函数
- **返回值**: 指令信息数组

```javascript
const calls = Instruction.filterInstructions(addr, 100, (insn) => {
  return insn.mnemonic === 'call';
});
```

#### `Instruction.filterInstructionsAsync(address, count, filter)`
异步过滤指令。
- **参数**: 同 `filterInstructions`
- **返回值**: Promise<指令信息数组>

```javascript
const results = await Instruction.filterInstructionsAsync(addr, 100, filter);
```

#### `Instruction.findXrefs(rangeStart, rangeSize, targetAddr)`
查找交叉引用。
- **参数**:
  - `rangeStart`: 范围起始地址（NativePointer）
  - `rangeSize`: 范围大小
  - `targetAddr`: 目标地址（NativePointer）
- **返回值**: 交叉引用结果数组

```javascript
const xrefs = Instruction.findXrefs(addr, 256, target);
xrefs.forEach(xref => {
  console.log(`${xref.address}: ${xref.type}`);
});
```

每个交叉引用对象包含：
- `address`: 引用地址（十六进制字符串）
- `type`: 引用类型（`'call'`、`'branch'` 或 `'data'`）
- `size`: 指令大小

---

## SoftwareBreakpoint API

软件断点（INT3 / BRK）。

### 方法

#### `SoftwareBreakpoint.set(address, callback)`
设置软件断点。
- **参数**:
  - `address`: 断点地址（NativePointer）
  - `callback`: 触发时的回调函数
- **返回值**: 断点对象

```javascript
const target = Module.findExportByName(null, 'malloc');
const bp = SoftwareBreakpoint.set(target, () => {
  console.log('Breakpoint hit!');
});
```

#### `bp.remove()`
移除断点。

```javascript
bp.remove();
```

#### `SoftwareBreakpoint.removeAll()`
移除所有软件断点。

```javascript
SoftwareBreakpoint.removeAll();
```

---

## HardwareBreakpoint API

硬件断点（调试寄存器）。

### 属性

#### `HardwareBreakpoint.maxBreakpoints`
最大硬件断点数量。
- **返回值**: number（通常为 4，不支持时为 0）

```javascript
const max = HardwareBreakpoint.maxBreakpoints;
console.log('Max hardware breakpoints:', max);
```

#### `HardwareBreakpoint.activeCount`
当前活动的硬件断点数量。
- **返回值**: number

```javascript
const count = HardwareBreakpoint.activeCount;
```

### 方法

#### `HardwareBreakpoint.set(address, type, size, callback)`
设置硬件断点/观察点。
- **参数**:
  - `address`: 地址（NativePointer）
  - `type`: 类型（`'execute'`、`'write'` 或 `'readwrite'`）
  - `size`: 大小（1、2、4 或 8 字节，execute 类型忽略）
  - `callback`: 触发时的回调函数
- **返回值**: 断点对象

```javascript
// 执行断点
const bp1 = HardwareBreakpoint.set(addr, 'execute', 1, () => {
  console.log('Execute breakpoint hit!');
});

// 写入观察点
const buf = Memory.alloc(8);
const bp2 = HardwareBreakpoint.set(buf, 'write', 4, () => {
  console.log('Write watchpoint hit!');
});
```

#### `bp.remove()`
移除硬件断点。

```javascript
bp.remove();
```

#### `HardwareBreakpoint.removeAll()`
移除所有硬件断点。

```javascript
HardwareBreakpoint.removeAll();
```

---

## MemoryAccessMonitor API

内存访问监控（基于页面保护）。

### 方法

#### `MemoryAccessMonitor.enable(ranges, callback)`
启用内存访问监控。
- **参数**:
  - `ranges`: 范围数组，每个包含 `address` 和 `size`
  - `callback`: 访问时的回调函数
- **返回值**: 监控器对象

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

回调函数接收的 `details` 对象包含：
- `address`: 访问地址（NativePointer）
- `pageBase`: 页面基址（NativePointer）
- `operation`: 操作类型（`'read'`、`'write'` 或 `'execute'`）
- `rangeIndex`: 触发的范围索引

#### `handle.disable()`
禁用监控器。

```javascript
handle.disable();
```

#### `MemoryAccessMonitor.disableAll()`
禁用所有监控器。

```javascript
MemoryAccessMonitor.disableAll();
```

#### `MemoryAccessMonitor.drain()`
处理待处理的访问事件。
- **返回值**: 处理的事件数量

```javascript
const count = MemoryAccessMonitor.drain();
console.log('Drained', count, 'events');
```

---

## ExceptionHandler API

异常处理器管理。

### 属性

#### `ExceptionHandler.isEnabled`
检查异常处理器是否已启用。
- **返回值**: boolean

```javascript
if (ExceptionHandler.isEnabled) {
  console.log('Exception handler is enabled');
}
```

### 方法

#### `ExceptionHandler.enable()`
启用全局异常处理器。

```javascript
ExceptionHandler.enable();
```

#### `ExceptionHandler.disable()`
禁用全局异常处理器。

```javascript
ExceptionHandler.disable();
```

---

## 工具函数

### `ptr(value)`
创建 NativePointer 的快捷方式。

```javascript
const p = ptr(0x1234);
const q = ptr('0xdeadbeef');
```

### `hexdump(address, options)`
生成十六进制转储。
- **参数**:
  - `address`: 起始地址（NativePointer）
  - `options`: 配置对象
    - `length`: 转储长度
- **返回值**: 十六进制转储字符串

```javascript
const buf = Memory.alloc(32);
const dump = hexdump(buf, { length: 32 });
console.log(dump);
```
