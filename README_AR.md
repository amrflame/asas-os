# Asas OS 1.0

**Asas OS** هو نظام تشغيل x86_64 صغير مبني من الصفر باستخدام C وC++ وAssembly.
كلمة Asas تعني "الأساس"، والمشروع يهدف إلى بناء طبقات نظام تشغيل حقيقية
وتعليمية من bootloader حتى kernel وGUI وإدارة الأقراص.

> المشروع تجريبي وبحثي. لا تستخدمه لحماية بيانات إنتاجية أو كبديل لنظام تشغيل
> يومي.

رابط المستودع العام المستهدف:

```text
https://github.com/amrflame/asas-os
```

## أهم المميزات

- إقلاع UEFI x86_64.
- Kernel مستقل وواجهة رسومية بسيطة باسم AsasGUI.
- Terminal وFile Manager داخل النظام.
- دعم Hyper-V Generation 2 كهدف تشغيل عام.
- دعم QEMU للاختبارات الآلية.
- VFS وMount Manager.
- FAT32 read/write، وNTFS/exFAT بعمليات كتابة محمية ومعاملات rollback.
- Disk Manager رسومي لعرض الأقراص والأقسام والحالة.
- دعم مسارات تخزين مثل VirtIO وAHCI وNVMe وUSB Mass Storage وHyper-V StorVSC.

## البناء

المسار الحالي Windows-first.

المتطلبات:

- Windows 10/11.
- PowerShell.
- Visual Studio 2022 C++ toolchain أو Build Tools.
- NASM في المسار المتوقع بواسطة `build.ps1`.

بناء ISO:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\New-AsasIso.ps1 -Build
```

بعد نشر المستودع يمكن نسخه بهذه الأوامر:

```powershell
git clone https://github.com/amrflame/asas-os.git
cd asas-os
```

## التشغيل على Hyper-V

الإعدادات المقترحة:

- Generation 2.
- Secure Boot disabled.
- Memory: 512 MB أو أكثر.
- DVD Drive: اربط ملف ISO الناتج من `build\releases`.

## المساهمة

اقرأ:

- `CONTRIBUTING.md` للإنجليزية.
- `CONTRIBUTING_AR.md` للعربية.
- `CODE_OF_CONDUCT.md` و`CODE_OF_CONDUCT_AR.md`.
- `SECURITY.md` و`SECURITY_AR.md` للبلاغات الأمنية.

أي مساهمة في التخزين أو أنظمة الملفات يجب أن تتضمن ملاحظات اختبار واضحة،
خصوصًا إذا كانت تلمس write path أو partition mutation أو repair.

## خارطة الطريق

- `ROADMAP.md`
- `ROADMAP_AR.md`
- `OS_DEVELOPMENT_PLAN_AR.md`
- `DISK_MANAGEMENT_PLAN_AR.md`

## الترخيص

المشروع مرخص تحت GNU General Public License v3.0 only. راجع `LICENSE`.

بالمساهمة في هذا المستودع، توافق أن تكون مساهمتك تحت نفس الترخيص.
