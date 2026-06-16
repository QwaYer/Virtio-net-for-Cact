# 🌐 Virtio-net-for-Cact

<p align="center">
  <img src="https://img.shields.io/badge/version-2.0.0-green.svg?style=for-the-badge" alt="Version: 2.0.0">
  <img src="https://img.shields.io/badge/license-GPLv3-blue.svg?style=for-the-badge" alt="License: GPLv3">
  <img src="https://img.shields.io/badge/arch-i686-red.svg?style=for-the-badge" alt="Arch: i686">
  <img src="https://img.shields.io/badge/format-cctk-green.svg?style=for-the-badge" alt="Output: virtio_net.cctk">
  <img src="https://img.shields.io/badge/bus-PCI-blue.svg?style=for-the-badge" alt="PCI virtio">
  <img src="https://img.shields.io/badge/irq-MSI--X-brightgreen.svg?style=for-the-badge" alt="MSI-X">
</p>

<p align="center">
  <strong>English.</strong> Legacy <strong>virtio-net</strong> PCI driver (I/O BAR0) → <strong><code>virtio_net.cctk</code></strong>.<br>
  <strong>2.0.0:</strong> migrated from PIC to <strong>MSI-X</strong>. Include paths updated from <code>Cact/kernel/net</code> → <code>Cact/net</code> to match kernel 2.0.0 directory layout.<br>
  <strong>Русский.</strong> Драйвер <strong>virtio-net</strong> → <strong><code>virtio_net.cctk</code></strong>.<br>
  <strong>2.0.0:</strong> переведён на <strong>MSI-X</strong>. Пути включения обновлены под новую структуру ядра.
</p>

---

## 🔨 Building

**Recommended — full workspace**

```sh
make -C CactOS-x86_32 iso
```

**Standalone**

```sh
make install   # auto-detects ../CactKernel-x86_32 and ../LocalRepoCactOS
make clean
```

Override paths if needed: `make KERN_ROOT=/custom/path LOCAL_REPO=/custom/path install`.
