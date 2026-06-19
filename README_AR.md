# Asas OS 1.0

**Asas OS** هو نظام تشغيل x86_64 صغير مبني من الصفر باستخدام C وC++ وAssembly.
كلمة Asas تعني "الأساس"، والمشروع ليس مجرد تجربة مغلقة، بل ورشة مفتوحة لأي
شخص يريد فهم وبناء وتحسين طبقات نظام تشغيل حقيقي: من UEFI boot إلى kernel
وواجهة رسومية وVFS وأنظمة ملفات وتعريفات تخزين وأدوات تطوير.

> المشروع تجريبي وبحثي. لا تستخدمه لحماية بيانات إنتاجية أو كبديل لنظام تشغيل
> يومي.

رابط المستودع العام المستهدف:

```text
https://github.com/amrflame/asas-os
```

![سطح مكتب Asas OS مع Terminal وFile Manager وSettings وAbout وDisk Manager](docs/assets/asas-os-desktop.png)

## تجربة سريعة Fast Start

عاوز تشوف Asas OS شغال بأسرع طريقة؟

1. انسخ المستودع:

```powershell
git clone https://github.com/amrflame/asas-os.git
cd asas-os
```

2. ابن ISO جاهز لـHyper-V/QEMU:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\New-AsasIso.ps1 -Build
```

3. ستجد الـISO الناتج هنا:

```text
build/releases/asas-os-*.iso
```

4. شغله على **Hyper-V Generation 2**:

- أنشئ VM من نوع Generation 2.
- عطّل Secure Boot.
- اربط ملف ISO الناتج في DVD Drive.
- شغّل الـVM.

5. داخل Asas OS جرّب:

```text
help
ls
disk rescan
fs info /
```

اختبار QEMU اختياري سريع:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\Run-QemuBootTest.ps1
```

## مساهمة سريعة Fast Contribute

عاوز تعمل أول مساهمة مفيدة بدون ما تتوه؟

1. اعمل Fork للمستودع على GitHub.
2. أنشئ branch:

```powershell
git checkout -b docs/first-improvement
```

3. اختر تغييرًا صغيرًا:

- حسّن فقرة في README أو الترجمة العربية.
- أضف مثالًا ناقصًا لأمر shell.
- أبلغ عن نتيجة تشغيل Hyper-V/QEMU.
- حسّن label أو رسالة خطأ في GUI.
- أضف ملاحظة اختبار صغيرة لسلوك storage أو filesystem.

4. ابن المشروع أو شغل أقرب تحقق:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\New-AsasIso.ps1 -Build
```

5. اعمل commit وpush:

```powershell
git add .
git commit -m "Describe the improvement"
git push origin docs/first-improvement
```

6. افتح Pull Request واكتب ما الذي اختبرته. لو التغيير أعمق، اقرأ
`CONTRIBUTING_AR.md` أولًا، خصوصًا ملاحظات أمان storage وfilesystems.

## لماذا تساهم؟

تطوير أنظمة التشغيل عادة يبدو بعيدًا ومغلقًا داخل مشاريع ضخمة. Asas OS يحاول
فتح الباب: الكود صغير بما يكفي للتعلم، وحقيقي بما يكفي ليعلمك هندسة أنظمة
جدية. مساهمتك قد تكون في boot path أو filesystem أو block device أو GUI أو
أمر shell أو اختبار أو توثيق.

المساهمات مرحب بها على كل المستويات:

- المساهمون الجدد يمكنهم تحسين التوثيق، الصور، help commands، الاختبارات،
  وتفاصيل GUI الصغيرة.
- مساهمو الأنظمة يمكنهم العمل على storage وVFS وfilesystems وscheduling
  وmemory وnetworking ومسارات الأجهزة.
- المختبرون يمكنهم تشغيل إعدادات VM مختلفة، تجربة disk images حقيقية، إرسال
  logs دقيقة، والمساعدة في تحويل التجارب إلى سلوك موثوق.

المشروع يقدّر الهندسة الهادئة: Pull Requests صغيرة، ملاحظات اختبار واضحة،
وفشل آمن عند الحالات غير المدعومة.

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

لو مش عارف تبدأ منين، دور على labels مثل `good first issue` و`help wanted`
و`docs` و`tests` و`gui` و`filesystem` و`storage`. ويمكنك فتح نقاش قصير عن
الجزء الذي تريد استكشافه وسنساعد في تحويله إلى أول patch آمن وواضح.

## خارطة الطريق

- `ROADMAP.md`
- `ROADMAP_AR.md`
- `OS_DEVELOPMENT_PLAN_AR.md`
- `DISK_MANAGEMENT_PLAN_AR.md`

## الترخيص

المشروع مرخص تحت GNU General Public License v3.0 only. راجع `LICENSE`.

بالمساهمة في هذا المستودع، توافق أن تكون مساهمتك تحت نفس الترخيص.
