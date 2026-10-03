# AppleVTD 逆向分析与兼容代码

日期：2026-10-03
分支：merge/tahoe
相关提交：be9aafb `itlwm: use device IOMapper for DMA mapping when AppleVTD is active`

## 1. 结论

- 当前机器 `config.plist` 中 `Kernel > Quirks > DisableIoMapper = false` 时，macOS 会初始化 `AppleVTD`。
- `itlwm` 旧 DMA 分配路径创建 `IODMACommand` 时没有传入设备自己的 `IOMapper`，只依赖系统默认 mapper；在 AppleVTD 开启时 `gen64IOVMSegments()` 无法得到合法的 IOVA，日志会打印：
  - `invalid AppleVTD DMA mapping (segments=..., addr=..., length=...)`
- 兼容做法：从 PCI 设备取得 `IOMapper`，显式传给 `IODMACommand::withSpecification()`。
- 该修改已在 `itlwm` target（Release x86_64）编译通过。

## 2. AppleVTD 在内核里的形态

- `AppleVTD` 不是 `/System/Library/Extensions` 下的独立 kext，而是内核内建类，在 `ioreg` 中注册为：
  - `+-o AppleVTD <class AppleVTD, id 0x100000131, registered, matched, active, ...>`
- 对第三方 kext 可见的是 `IOMapper` 接口，关键符号由 `/System/Library/Kernels/kernel` 导出：
  - `IOMapper::copyMapperForDevice(IOService*)`
  - `IOMapper::copyMapperForDeviceWithIndex(IOService*, unsigned int)`
  - `IOMapper::gSystem`
  - `IOMapper::waitForSystemMapper()`
  - `IODMACommand::withSpecification(...)`
  - `IODMACommand::genIOVMSegments(...)`

## 3. `IOMapper::copyMapperForDevice` 逆向

`nm` 地址：`ffffff80009ef270`

反汇编（关键部分）：

```asm
IOMapper::copyMapperForDevice(IOService*):
    xorl %esi, %esi
    jmp  IOMapper::copyMapperForDeviceWithIndex(IOService*, unsigned int)

IOMapper::copyMapperForDeviceWithIndex(IOService*, unsigned int):
    ...
    leaq "iommu-parent"(%rip), %rsi
    callq *0x2e8(%rax)        ; 读取 IOService 的 "iommu-parent" 属性
    testq %rax, %rax
    je   fallback
    ...                        ; 检查属性对象是否属于 IOMapper / OSData
    ...                        ; 在 provider 链上找到对应 mapper
    ...
```

结论：
- 该函数返回一个 **retained** mapper（引用计数 +1）。
- 调用方用完必须 `release()`。
- 如果设备树中没有 `iommu-parent`，则走系统 mapper 回退路径。

## 4. `IODMACommand` 如何保存 mapper

`IODMACommand::withSpecification(...)` 地址：`ffffff80009edcf0`

关键流程：

```asm
IODMACommand::withSpecification(...):
    leaq IODMACommand::gMetaClass(%rip), %rdi
    callq *0x88(%rax)          ; alloc IODMACommand 对象
    ...
    callq *0x120(%r10)         ; initWithSpecification(...)
    ...
```

`IODMACommand::setSpecification(...)` 地址：`ffffff80009ec390`

```asm
    ...
    movq 0x40(%rbx), %r15      ; 旧 mapper
    cmpq %r15, %r8
    je   skip
    movq %r8, 0x40(%rbx)       ; 保存新 mapper 到 this+0x40
    testq %r8, %r8
    je   skip_retain
    callq *0x20(%rax)          ; retain 新 mapper
    ...
    callq *0x28(%rax)          ; release 旧 mapper
```

结论：
- `IODMACommand` 会 **retain 传入的 mapper**。
- 因此驱动在 `withSpecification()` 成功后可以立即 `release()` 自己那份引用。
- 这也是我们代码里“获取 mapper -> 创建 DMA command -> release mapper”的依据。

## 5. 旧代码为什么和 AppleVTD 不兼容

旧实现（三个 HAL 相同）：

```cpp
IODMACommand *cmd = IODMACommand::withSpecification(
    kIODMACommandOutputHost64,
    64, 0,
    IODMACommand::kMapped,
    0, alignment);          // mapper 参数缺省 = NULL
```

当 mapper 为 NULL 时，`setSpecification` 会尝试使用 `IOMapper::gSystem`。在 AppleVTD 已初始化的机器上，设备实际需要的是它自己的 `iommu-parent` mapper；依赖系统 mapper 导致：
- `gen64IOVMSegments()` 返回错误，或
- 返回的 `seg.fIOVMAddr` / `seg.fLength` 不满足检查。

于是驱动直接放弃 DMA 内存分配，Wi-Fi 无法初始化。

## 6. 兼容代码

### 6.1 修改后的 DMA 分配函数（以 iwx 为例）

```cpp
bool allocDmaMemory2(struct iwx_dma_info *dma, size_t size, int alignment,
                     IOMapper *mapper)
{
    IOBufferMemoryDescriptor *bmd;
    IODMACommand::Segment64 seg;
    UInt64 ofs = 0;
    UInt32 numSegs = 1;

    bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache,
        size, DMA_BIT_MASK(64));

    if (bmd == NULL)
        return false;
    if (bmd->prepare() != kIOReturnSuccess) {
        bmd->release();
        return false;
    }

    IODMACommand *cmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 0,
        IODMACommand::kMapped, 0, alignment,
        mapper);                       // 关键：显式设备 mapper

    if (cmd == NULL) {
        bmd->complete();
        bmd->release();
        return false;
    }

    if (cmd->setMemoryDescriptor(bmd) != kIOReturnSuccess ||
        cmd->gen64IOVMSegments(&ofs, &seg, &numSegs) != kIOReturnSuccess ||
        numSegs != 1 || seg.fIOVMAddr == 0 || seg.fLength < size) {
        XYLog("%s invalid AppleVTD DMA mapping ...\n", __FUNCTION__, ...);
        cmd->release();
        bmd->complete();
        bmd->release();
        return false;
    }

    dma->paddr = seg.fIOVMAddr;
    dma->vaddr = bmd->getBytesNoCopy();
    dma->size  = size;
    dma->buffer = bmd;
    dma->cmd    = cmd;
    memset(dma->vaddr, 0, dma->size);
    return true;
}
```

### 6.2 设备 mapper 获取与释放

```cpp
int ItlIwx::iwx_dma_contig_alloc(bus_dma_tag_t tag,
                                 struct iwx_dma_info *dma,
                                 bus_size_t size, bus_size_t alignment)
{
    IOMapper *mapper = pci.pa_tag
        ? IOMapper::copyMapperForDevice(pci.pa_tag) : NULL;
    bool ok = allocDmaMemory2(dma, size, alignment, mapper);
    if (mapper)
        mapper->release();
    return ok ? 0 : 1;
}
```

`iwm`、`iwn` 采用完全相同的模式。

## 7. 已修改文件

- `itlwm/hal_iwx/ItlIwx.cpp`
- `itlwm/hal_iwm/io.cpp`
- `itlwm/hal_iwn/ItlIwn.cpp`

每个文件都：
1. `#include <IOKit/IOMapper.h>`
2. 给 `allocDmaMemory2()` 增加 `IOMapper *mapper` 参数
3. `withSpecification(..., mapper)`
4. 在 `*_dma_contig_alloc()` 中获取并释放 mapper

## 8. 验证状态

- [x] `itlwm` target Release x86_64 编译通过
- [ ] AppleVTD 开启时的实机验证（待与 P1 修复合并后部署）
- [ ] 检查 `sudo dmesg | grep -iE 'AppleVTD|invalid AppleVTD DMA mapping'`

## 9. 下一步建议

1. 先确认恢复后的 Wi-Fi 稳定。
2. 修复 P1 的 `IO80211VirtualInterface` 未导出符号问题。
3. 将“P1 修复 + AppleVTD mapper 兼容”合并，构建 `AirportItlwm-Tahoe` Release x86_64。
4. 把 `config.plist` 的 `DisableIoMapper` 暂时保持 `true` 部署一次，确认稳定。
5. 如要重新打开 AppleVTD，把 `DisableIoMapper` 改回 `false`，再部署含 mapper 修复的 kext 验证。
