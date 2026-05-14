# 🌐 Virtio-net-for-Cact

<p align="center">
  <img src="https://img.shields.io/badge/license-GPLv3-blue.svg?style=for-the-badge" alt="License: GPLv3">
  <img src="https://img.shields.io/badge/arch-i686-red.svg?style=for-the-badge" alt="Arch: i686">
  <img src="https://img.shields.io/badge/format-cctk-green.svg?style=for-the-badge" alt="Output: virtio_net.cctk">
  <img src="https://img.shields.io/badge/bus-PCI-blue.svg?style=for-the-badge" alt="PCI virtio">
</p>

<p align="center">
  <strong>English.</strong> Legacy <strong>virtio-net</strong> PCI driver (I/O BAR0) → <strong><code>virtio_net.cctk</code></strong>.<br>
  <strong>Русский.</strong> Драйвер <strong>virtio-net</strong> → <strong><code>virtio_net.cctk</code></strong>.
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
