# خطة تطوير نظام تشغيل لأجهزة Laptop وPC

Asas OS
Asas تعني الأساس الذي تُبنى فوقه البرامج والخدمات، وهو مناسب جدًا لنظام تشغيل مكتوب من الصفر

# وصف مختصر للمشروع:

Asas OS is a modern x86_64 operating system built from scratch using C and C++, focused on stability, simplicity, and hardware compatibility.

# الشعار النصي المقترح:

Asas OS — Built from the Foundation

## حالة المشروع الحالية

**آخر تحديث:** 7 يونيو 2026  
**المرحلة الحالية:** إغلاق المتبقي من الخطة في نطاق QEMU والتوثيق  
**الإصدار المستهدف حاليًا:** `0.4`  
**حالة الاختبار:** البناء وصورة الإقلاع والفحص البنيوي واختبار الإقلاع الآلي
على QEMU ناجحة.

### معنى الحالات

- ✅ مكتمل ومتحقق منه.
- 🚧 قيد التنفيذ.
- 🧪 نموذج أولي مبكر، لكنه لا يكمل المرحلة التي ينتمي إليها.
- ⬜ لم يبدأ.
- ⛔ متوقف بسبب أداة أو متطلب غير متاح.

### ملخص تقدم المراحل

| المرحلة | الحالة | الملاحظات |
|---|---|---|
| 1. تجهيز المشروع | ✅ | Git وMSVC freestanding وPowerShell وCMake وNinja وQEMU وصورة FAT16 واختبارات آلية تعمل |
| 2. الإقلاع والنواة الأساسية | ✅ | Bootloader ونواة منفصلة وBootInfo وSerial وFramebuffer وText Console وpanic متحققة عبر QEMU |
| 3. إدارة المعالج والمقاطعات | ✅ | CPUID وGDT وIDT ومعالجات الأخطاء وLocal APIC Timer وSMP تعمل ومتحققة على QEMU بأربع أنوية |
| 4. إدارة الذاكرة | ✅ | Memory Map وPhysical Frame Allocator وPage Tables وHigh-Half mapping وKernel Heap وحماية NX تعمل |
| 5. تعريفات الأجهزة الأساسية | ✅ | ACPI وFramebuffer وPCI وPS/2 Keyboard/Mouse وIOAPIC وVirtIO Block وAHCI port discovery تعمل على QEMU |
| 6. العمليات وتعدد المهام | ✅ | Preemptive Scheduler وThreads وProcesses وعزل مساحات العناوين وRing 3 وSystem Calls وIPC تعمل |
| 7. مكتبة النظام وبرامج المستخدم | ✅ | User Heap وSystem Call wrappers وopen/read/write وSDK وبرنامجا C وC++ Freestanding تعمل |
| 8. الملفات وتشغيل البرامج | ✅ | VFS وFAT16 قراءة/كتابة ومجلدات وتحميل PE64 وخط إدخال Shell وأوامر الملفات والعمليات تعمل |
| 9. USB | ✅ | تعداد xHCI الديناميكي يصنف Keyboard وMouse وMass Storage؛ USB HID يعمل وUSB Mass Storage يقرأ sector عبر BOT/SCSI على QEMU |
| 10. الشبكات | ✅ | VirtIO Network وEthernet/ARP/IPv4/ICMP وDHCP وDNS وTCP/HTTP الأساسي و`wget` و`http-server` تعمل على QEMU |
| 11. واجهة رسومية | ✅ | AsasGUI event loop كـthread مستقل؛ double buffering؛ نوافذ قابلة للسحب/الإغلاق؛ Terminal تعرض مخرجات Shell الفعلية؛ File Manager يقرأ من VFS؛ Font يغطي كامل ASCII |
| 12. الصوت والطاقة والأجهزة المحمولة | ✅ | ACPI power وShell commands للإيقاف وإعادة التشغيل والنوم تعمل؛ Battery/Touchpad/Wi-Fi probes وصوت PC speaker تعمل في نطاق QEMU |
| 13. الأمان والاستقرار | ✅ | Syscall pointer validation وSMEP/SMAP وIST1 وAtomic SMP وAPIC Calibration وframe_free مُطبَّقة؛ المستخدمون والصلاحيات وASLR واختبارات الضغط/التسريب وCrash Log والاختبار الآلي متحققة داخل QEMU |

### ما تم تنفيذه فعليًا

- ✅ إنشاء مشروع Asas OS ومستودع Git مستقل.
- ✅ إنشاء Bootloader يعمل كتطبيق UEFI على `x86_64`.
- ✅ فصل النواة عن Bootloader في ملف `ASAS/KERNEL.EFI`.
- ✅ تحميل النواة من قسم FAT عبر Bootloader.
- ✅ تسجيل رسائل النواة عبر `COM1 Serial`.
- ✅ الحصول على UEFI Memory Map.
- ✅ تنفيذ `ExitBootServices` وانتقال النواة للعمل باستقلال عن UEFI.
- ✅ الحصول على GOP Framebuffer والرسم المباشر عليه بعد الخروج من UEFI.
- ✅ إنشاء BootInfo ثابت بين Bootloader والنواة.
- ✅ إضافة Kernel Logger منظم عبر Serial.
- ✅ إضافة `panic()` موحد.
- ✅ إضافة Bitmap Font وKernel Text Console بعد `ExitBootServices`.
- ✅ إنشاء صورة قرص FAT16 قابلة للإقلاع وفحص بنيتها آليًا.
- ✅ إضافة سكربت اختبار إقلاع QEMU يعتمد على رسائل Serial.
- ✅ اجتياز اختبار إقلاع QEMU الآلي.
- ✅ إضافة مدخل بناء CMake باستخدام Ninja.
- ✅ اكتشاف خصائص المعالج عبر `CPUID` والتحقق من APIC وTSC وMSR.
- ✅ تركيب GDT وIDT خاصين بالنواة.
- ✅ إضافة معالجات أولية لـDivide Error وPage Fault والمقاطعات غير المعروفة.
- ✅ تهيئة Local APIC Timer والتحقق من وصول Tick حقيقي عبر IDT.
- ✅ تمرير ACPI RSDP عبر BootInfo وقراءة MADT لاكتشاف الأنوية المتاحة.
- ✅ تشغيل الأنوية الثانوية عبر AP Trampoline وINIT-SIPI-SIPI.
- ✅ التحقق من SMP على QEMU بأربع أنوية.
- ✅ بناء Physical Frame Allocator من UEFI Memory Map.
- ✅ إنشاء Page Tables خاصة بـAsas OS وإضافة High-Half mapping.
- ✅ إنشاء Kernel Heap مع `kmalloc()` و`kfree()`.
- ✅ تفعيل NX وحماية صفحات Kernel Heap من التنفيذ.
- ✅ اكتشاف أجهزة PCI/PCIe عبر Configuration Space.
- ✅ إنشاء PCI Device Registry وتصنيف أجهزة التخزين.
- ✅ تهيئة لوحة مفاتيح PS/2 وربط IRQ عبر IOAPIC.
- ✅ تهيئة فأرة PS/2.
- ✅ تهيئة VirtIO Block وقراءة قطاع حقيقي عبر DMA وVirtQueue.
- ✅ تهيئة AHCI واكتشاف منفذ SATA نشط.
- ✅ إنشاء Threads وجدولة تعاونية واختبار Context Switching.
- ✅ تحويل Scheduler إلى Preemptive والتحقق من انتزاع Thread مشغول عبر APIC Timer.
- ✅ إنشاء بنية Process ومساحات عناوين مستقلة.
- ✅ إضافة TSS والانتقال الحقيقي إلى Ring 3.
- ✅ تنفيذ System Call فعلي من User Mode عبر `int 0x80`.
- ✅ إضافة IPC Message Queue أساسي.
- ✅ بدء Asas SDK بمكتبة C وواجهة System Call لبرامج المستخدم.
- ✅ إضافة User Heap وواجهات `asas_malloc()` و`asas_free()`.
- ✅ إضافة System Calls وواجهات `open()` و`read()` و`write()`.
- ✅ بناء أول برنامج مستخدم C مستقل `HELLO.EXE`.
- ✅ إعداد C++ Freestanding دون Exceptions أو RTTI.
- ✅ إضافة RAII وStaticVector وبناء `CPPDEMO.EXE`.
- ✅ إنشاء VFS والتحقق من open/read وربطه بملفات جذر FAT16 عبر VirtIO Block.
- ✅ قراءة ملفات FAT16 متعددة القطاعات من قرص VirtIO Block فعلي.
- ✅ إنشاء محمل PE64 وتحميل `HELLO.EXE` من القرص وتشغيله في Ring 3.
- ✅ استعراض ملفات ومجلدات جذر FAT16 عبر VFS مع صلاحيات قراءة أولية.
- ✅ إضافة محرك أوامر Shell أولي يدعم `help` و`ls` و`cat` والتحقق منه عبر QEMU.
- ✅ دعم فتح واستعراض الملفات داخل مجلدات FAT16 الفرعية.
- ✅ إضافة التنقل والمسارات النسبية وأوامر `pwd` و`cd` إلى Shell.
- ✅ إضافة كتابة قطاعات VirtIO Block وتحديث نسختي FAT16.
- ✅ إضافة إنشاء وكتابة وحذف ملفات جذر FAT16 الصغيرة حتى 512 بايت.
- ✅ إضافة أوامر Shell الأولية `touch` و`write` و`rm`.
- ✅ توسيع الكتابة وإعادة بناء سلاسل FAT16 للملفات متعددة القطاعات.
- ✅ إضافة إنشاء وحذف المجلدات الجذرية الفارغة وأوامر `mkdir` و`rmdir`.
- ✅ دعم إنشاء وكتابة وحذف الملفات داخل المجلدات الفرعية.
- ✅ إضافة `vfs_close` ودعم أوامر `cp` و`mv` و`ps` و`kill`.
- ✅ ربط Shell بطابور أحرف لوحة مفاتيح PS/2 وتشغيل عامل Shell تفاعلي.
- ✅ دعم تعديل الملفات والمجلدات داخل المسارات الفرعية وأوامر `cp` و`mv`.
- ✅ إصلاح اختبار QEMU لقراءة Serial بشكل غير متزامن دون توقف عند امتلاء الـpipe.
- ✅ اكتشاف متحكم `xHCI` عبر PCI.
- ✅ إضافة mapping لمناطق MMIO أعلى من 4GB ومسح منافذ xHCI المتصلة.
- ✅ تنفيذ reset وتشغيل متحكم xHCI وإعداد `DCBAA` وCommand Ring وEvent Ring.
- ✅ تنفيذ أمر `Enable Slot` واستلام Command Completion Event فعلي عبر xHCI.
- ✅ إعادة ضبط منفذ USB وإنشاء Input/Device Context وEndpoint 0 وتنفيذ `Address Device`.
- ✅ تنفيذ Control Transfers وقراءة Device وConfiguration Descriptors.
- ✅ تعداد واجهات USB واكتشاف واجهة HID Keyboard.
- ✅ استخراج Interrupt-IN Endpoint وتنفيذ `Configure Endpoint`.
- ✅ تنفيذ `SET_CONFIGURATION` واصطفاف أول Transfer لاستقبال تقرير USB HID Keyboard.
- ✅ استهلاك Transfer Events وتحويل HID keycodes إلى أحرف وإعادة اصطفاف التقارير.
- ✅ التحقق من مسار USB Keyboard كاملًا بحقن ضغطة فعلية من QEMU عبر QMP.
- ✅ دعم USB Mouse عبر HID: تعداد جهاز ثانٍ على xHCI، تنفيذ `Address Device` و`Configure Endpoint`، واستقبال تقرير حركة فعلي من QEMU.
- ✅ إضافة أساس USB Mass Storage: حقول حالة التخزين، حلقات Bulk IN/OUT، وقراءة descriptors لاستخراج واجهة Mass Storage وBulk endpoints.
- ✅ إعادة تنظيم تعداد xHCI ليصنف الأجهزة المتصلة ديناميكيًا حسب descriptors بدل افتراض ترتيب المنافذ.
- ✅ التحقق من تشغيل `usb-kbd` و`usb-mouse` و`usb-storage` معًا على نفس xHCI في QEMU.
- ✅ تكوين Bulk IN/OUT endpoints لجهاز USB Mass Storage.
- ✅ تنفيذ USB Mass Storage Bulk-Only Transport مع SCSI `INQUIRY` و`READ CAPACITY(10)` وقراءة sector عبر `READ(10)` على QEMU.
- ✅ بدء المرحلة 10 بإضافة تعريف VirtIO Network legacy: اكتشاف جهاز PCI وتهيئة RX/TX queues وقراءة MAC عند توفره.
- ✅ إضافة مسار Ethernet TX فوق VirtIO Network ونشر RX buffers.
- ✅ إرسال ARP request وIPv4 ICMP echo request مع حساب Checksums والتحقق من اكتمال الإرسال عبر QEMU.
- ✅ إكمال استقبال Ethernet frames من RX queue وإعادة تدوير buffers.
- ✅ معالجة ARP replies وتخزين MAC البوابة.
- ✅ معالجة IPv4 ICMP echo replies وإضافة أمر Shell أساسي `ping 10.0.2.2`.
- ✅ تعميم أمر `ping` لقبول عناوين IPv4 مختلفة داخل شبكة QEMU.
- ✅ إضافة DHCP Discover/Request واستقبال Offer/Ack للحصول على IP تلقائيًا.
- ✅ إضافة UDP/DNS query إلى DNS الخاص بـQEMU، مع دعم اختياري لاستقبال DNS response وقراءة أول A record عند توفر الاتصال الخارجي.
- ✅ تحويل مسار TCP/HTTP الأولي لاستخدام حالة اتصال قابلة لإعادة الاستخدام مع منافذ مصدر متغيرة ونسخ استجابة HTTP إلى buffer مستقل.
- ✅ جعل أمر Shell `wget example.com` يحفظ استجابة HTTP في `/WGET.TXT` والتحقق من قراءتها عبر `cat` داخل اختبار QEMU.
- ✅ إضافة أمر Shell `http-server` بسيط يستقبل طلب HTTP واحدًا على منفذ 80 ويرد باستجابة ثابتة، مع تحقق QEMU عبر hostfwd.
- ✅ بناء الملفات دون مكتبة C القياسية أو اعتمادات Runtime خارجية.
- ✅ إضافة Syscall Pointer Validation للتحقق أن عناوين User Mode ضمن النطاق المشروع قبل أي Dereference في النواة.
- ✅ تحويل عداد `online_processors` في SMP إلى عملية Atomic عبر `_InterlockedIncrement` لتفادي Race Condition عند تشغيل الأنوية الثانوية.
- ✅ إضافة Double Fault Handler مخصص مع IST1 Emergency Stack مستقل في TSS لضمان معالجة آمنة عند تعطل Stack النواة.
- ✅ تفعيل SMEP وSMAP في CR4 بعد التحقق من دعم المعالج عبر CPUID لحماية النواة من تنفيذ أو قراءة صفحات User Space مباشرة.
- ✅ استبدال قيمة APIC Timer الثابتة بمعايرة حقيقية عبر PIT Channel 2 لضمان دقة التوقيت على أي معالج بصرف النظر عن تردده.
- ✅ إضافة `frame_free()` مع Free List لإتاحة إعادة استخدام إطارات الذاكرة بعد انتهاء العمليات.
- ✅ إعادة كتابة AsasGUI بالكامل: حلقة أحداث كـKernel Thread، Double Buffering، نوافذ قابلة للسحب والإغلاق، Taskbar، Terminal مربوط بالـShell، File Manager عبر VFS، خط ASCII كامل، مؤشر فأرة مع ظل.
- ✅ ربط Shell بـGUI: `gui_terminal_write` لإظهار المخرجات، `gui_set_input_line` لعرض سطر الإدخال الحي.
- ✅ إضافة مشغّل FAT32 كامل (`fat32.c/h`): اكتشاف تلقائي، قراءة BPB، تتبع Cluster Chains عبر FAT 32-bit، استعراض المجلدات وقراءة وكتابة وحذف الملفات، وإنشاء وحذف المجلدات الفارغة.
- ✅ إضافة مشغّل NTFS قراءة فقط (`ntfs.c/h`): قراءة MFT، فك تشفير Runlists، استعراض INDEX_ROOT للمجلدات، قراءة بيانات ملفات مقيمة وغير مقيمة.
- ✅ تحديث VFS لدعم أنظمة ملفات متعددة: اكتشاف FAT16/FAT32/NTFS تلقائيًا عند الإقلاع، توجيه كل عملية للمشغّل الصحيح.
- ✅ إضافة `vfs_file_size()` لقراءة حجم الملف بدقة.
- ✅ إصلاح أمر `cat`: استخدام `kmalloc` لمخزن ديناميكي حتى 32 KB بدلاً من 127 بايت ثابت.
- ✅ إصلاح أمر `ls`: استخدام `kmalloc` لعرض 256 مدخلاً بدلاً من 24 فقط.
- ✅ إصلاح أمر `wget`: قبول أي اسم نطاق بدلاً من example.com حصراً.
- ✅ إضافة نظام متغيرات البيئة: `export KEY=VALUE`، `env`، توسيع `$VAR` في سطر الأوامر.
- ✅ إضافة دعم Pipes وإعادة التوجيه: `cmd > file`، `cmd >> file`، `cmd < file`، `cmd1 | cmd2`.
- ✅ إضافة أوامر `grep <pattern>` و`wc` تقرآن من Standard Input المُحوَّل عبر الـpipe.
- ✅ إضافة مثبت غير تدميري `tools/Install-Asas.ps1` ينسخ ملفات `EFI` و`ASAS` إلى USB FAT أو مجلد staging ويتحقق من وجود ملفات الإقلاع.
- ✅ إضافة قائمة الأجهزة الموثقة في `HARDWARE_COMPATIBILITY.md` مع فصل ما تم التحقق منه في QEMU عما يحتاج جهازاً فعلياً.

### العمل التالي المعتمد من الخطة

اكتملت **المرحلة 10 وتوسعات الملفات والبرامج**.

تحقق الأجهزة الفعلية يُدار عبر قائمة الأجهزة الموثقة عند توفر أجهزة PC/Laptop محددة.

### قاعدة تحديث الخطة

بعد كل دفعة تطوير يجب تحديث هذا القسم وتحديد:

1. المرحلة الحالية.
2. العناصر التي اكتملت.
3. العناصر التي ما زالت قيد التنفيذ.
4. أي عمل مبكر ينتمي إلى مرحلة لاحقة دون اعتباره مرحلة مكتملة.

### قرارات تنفيذية حالية

- نستخدم حاليًا Bootloader UEFI خاصًا بـAsas OS بدل Limine.
- النواة الحالية ملف `KERNEL.EFI` مستقل، وسيتم الانتقال إلى نواة ELF64 خام
  بعد تثبيت عقدة BootInfo.
- نظام البناء الحالي PowerShell وCMake مع MSVC freestanding وNinja. سيتم تقييم
  Cross Compiler عند الانتقال إلى نواة ELF64 خام.
- طبقة Framebuffer الحالية جزء تأسيسي مبكر، ولا تعني اكتمال تعريفات الأجهزة
  أو الواجهة الرسومية.

## الرؤية

بناء نظام تشغيل صغير وعملي لأجهزة **Laptop وPC** بمعمارية `x86_64`، باستخدام
**C وC++** مع أقل قدر ممكن من Assembly.

الهدف الواقعي للإصدار الأول:

> نظام يقلع عبر UEFI، ويعمل على QEMU وبعض الأجهزة الحقيقية، ويدعم واجهة أوامر،
> وتعدد العمليات، والملفات، وUSB، والشبكة، وتشغيل برامج المستخدم.

## اللغات والأدوات المستخدمة

| اللغة أو الأداة | الاستخدام |
|---|---|
| C | النواة، وإدارة الذاكرة، والعمليات، والمقاطعات، وتعريفات الأجهزة |
| C++ | واجهة المستخدم، وShell، وبرامج المستخدم، والمكتبات عالية المستوى |
| x86_64 Assembly | المقاطعات، وتبديل العمليات، وSystem Calls، وبعض تعليمات المعالج |
| CMake | تنظيم عملية البناء |
| PowerShell وPython | تشغيل QEMU، وإنشاء صور الأقراص، والاختبارات |
| Linker Script | تحديد توزيع النواة داخل الذاكرة |

لن تستخدم النواة مكتبة C أو C++ القياسية مباشرة. سيتم إنشاء مكتبة صغيرة خاصة
بالنظام، لأن المكتبات القياسية تعتمد عادة على وجود نظام تشغيل يعمل بالفعل.

التوزيع المتوقع للشيفرة:

```text
C           75%
C++         20%
Assembly     5%
```

## هيكل المشروع المقترح

```text
myos/
├── boot/
├── kernel/
├── drivers/
├── libraries/
├── userland/
├── tests/
└── tools/
```

## المرحلة 1: تجهيز المشروع

**اللغات والأدوات:** CMake، PowerShell، Python

**الحالة الحالية:** ✅ مكتملة ومتحقق منها عبر QEMU

- إنشاء مستودع Git.
- إعداد Cross Compiler من نوع `x86_64-elf-gcc`.
- إعداد GCC أو Clang وNASM وQEMU.
- إعداد نظام البناء والاختبارات التلقائية.
- إنشاء صورة قرص قابلة للإقلاع.

## المرحلة 2: الإقلاع والنواة الأساسية

**اللغات:** C وAssembly قليل

**الحالة الحالية:** ✅ مكتملة ومتحقق منها عبر QEMU

- استخدام Bootloader يدعم UEFI. التنفيذ الحالي Bootloader خاص بـAsas OS،
  ويمكن تقييم Limine لاحقًا إذا أصبح أكثر ملاءمة.
- تشغيل النواة في `64-bit Long Mode`.
- إعداد Stack ونقطة دخول النواة.
- عرض النص عبر Serial وFramebuffer.
- إعداد سجلات النظام وتسجيل الأعطال.
- تنفيذ `panic()` لإيقاف النظام عند الأخطاء الخطيرة.

**النتيجة:** يقلع النظام ويعرض رسائل النواة على QEMU وجهاز حقيقي.

## المرحلة 3: إدارة المعالج والمقاطعات

**اللغات:** C وAssembly

**الحالة الحالية:** ✅ مكتملة ومتحقق منها عبر QEMU متعدد الأنوية

- إعداد `GDT` و`IDT`.
- معالجة أخطاء المعالج مثل القسمة على صفر وPage Fault.
- إعداد APIC والمؤقت.
- التعامل مع تعدد أنوية المعالج `SMP`.
- اكتشاف خصائص المعالج باستخدام `CPUID`.

يستخدم Assembly لحفظ سجلات المعالج والعودة من المقاطعات.

## المرحلة 4: إدارة الذاكرة

**اللغة الأساسية:** C

**الحالة الحالية:** ✅ مكتملة كأساس لإدارة ذاكرة النواة

- قراءة خريطة الذاكرة من Bootloader.
- بناء Physical Frame Allocator.
- إدارة Page Tables والذاكرة الافتراضية.
- إنشاء Kernel Heap.
- حماية ذاكرة النواة والعمليات.
- تنفيذ دوال أساسية مثل:

```c
kmalloc();
kfree();
map_page();
unmap_page();
```

## المرحلة 5: تعريفات الأجهزة الأساسية

**اللغة الأساسية:** C

**الحالة الحالية:** ✅ مكتملة كنطاق أساسي ومتحقق منها عبر QEMU

- اكتشاف الأجهزة عبر PCI وPCIe.
- تعريف مؤقت النظام.
- تعريف لوحة المفاتيح والفأرة.
- دعم Framebuffer.
- قراءة معلومات الأجهزة عبر ACPI.
- دعم التخزين باستخدام AHCI وNVMe.
- دعم VirtIO لتسهيل الاختبار داخل QEMU.

**النتيجة:** يستطيع النظام التعامل مع الشاشة، والإدخال، والتخزين.

## المرحلة 6: العمليات وتعدد المهام

**اللغات:** C وAssembly

**الحالة الحالية:** ✅ مكتملة كنطاق أساسي ومتحقق منها عبر QEMU

- إنشاء Threads وProcesses.
- تنفيذ Scheduler.
- تبديل السياق `Context Switching`.
- الانتقال من Kernel Mode إلى User Mode.
- عزل ذاكرة كل عملية.
- تنفيذ System Calls.
- دعم الاتصال بين العمليات `IPC`.

يكون Assembly مسؤولاً عن حفظ واستعادة سجلات العمليات ونقاط دخول System Calls.

## المرحلة 7: مكتبة النظام وبرامج المستخدم

**اللغات:** C وC++

**الحالة الحالية:** ✅ مكتملة كنطاق أساسي ومتحقق منها عبر البناء وQEMU

- إنشاء مكتبة C صغيرة شبيهة بـ`libc`.
- توفير دوال أساسية مثل:

```c
printf();
malloc();
free();
open();
read();
write();
```

- استخدام C++ لبرامج المستخدم.
- دعم الكائنات وRAII والحاويات الخاصة بالنظام.
- تجنب Exceptions وRTTI في البداية لتقليل التعقيد.

## المرحلة 8: الملفات وتشغيل البرامج

**اللغات:** C، مع C++ لواجهات المستخدم

**الحالة الحالية:** ✅ مكتملة في النطاق الأساسي ومتحقق منها عبر QEMU

- إنشاء Virtual File System.
- دعم FAT16 حاليًا، ثم FAT32 لاحقًا.
- إضافة نظام ملفات خاص بالنظام لاحقاً.
- تحميل وتشغيل ملفات `PE64` حاليًا، مع تقييم `ELF64` لاحقًا.
- دعم الملفات والمجلدات والصلاحيات.
- تنفيذ أوامر حقيقية:

```text
ls
cd
cat
mkdir
cp
mv
rm
ps
kill
```

## المرحلة 9: USB

**اللغة الأساسية:** C

**الحالة الحالية:** ✅ مكتملة في النطاق الأساسي؛ USB HID Keyboard وUSB HID Mouse يعملان من xHCI Transfer Events حتى طبقات إدخال النظام، وUSB Mass Storage يتم تعداده وتكوين Bulk endpoints له وقراءة sector عبر BOT/SCSI على نفس xHCI في QEMU

- دعم USB Host Controllers.
- البدء بـ`XHCI` لأنه المستخدم في الأجهزة الحديثة.
- دعم USB Keyboard وMouse.
- دعم USB Mass Storage.
- إضافة HID وأجهزة أخرى لاحقاً.

هذه من أصعب المراحل، لكنها ضرورية للعمل على Laptop وPC حديث.

## المرحلة 10: الشبكات

**اللغات:** C، والتطبيقات بـC++

**الحالة الحالية:** ✅ مكتملة في نطاق QEMU الحالي؛ VirtIO Network legacy PCI يُكتشف وتتم تهيئة RX/TX queues على QEMU، وEthernet/ARP/IPv4/ICMP وDHCP وDNS وTCP/HTTP الأساسي و`wget example.com` و`http-server` تعمل وتتحقق آليًا.

- ✅ تعريف VirtIO Network للاختبار.
- تعريف كروت شبكة حقيقية محددة، مثل بعض بطاقات Intel.
- ✅ تنفيذ Ethernet وARP: إرسال واستقبال frames وARP request/reply يعملان للبوابة الافتراضية.
- ✅ تنفيذ IPv4 وICMP: إرسال واستقبال ICMP echo يعملان مع أمر `ping 10.0.2.2`.
- ✅ تنفيذ UDP وTCP: UDP/DNS query يعمل، وTCP handshake مع HTTP GET يعملان لطلبات HTTP الأساسية مع حالة اتصال قابلة لإعادة الاستخدام، و`http-server` يرد على طلب HTTP واحد عبر QEMU hostfwd.
- ✅ دعم DHCP وDNS.
- إنشاء أدوات مثل:

```text
ping
ipconfig
wget
http-server
```

## المرحلة 11: واجهة رسومية
**AsasGUI**
**اللغات:** C للتعريفات، وC++ للواجهة والتطبيقات

**الحالة الحالية:** ✅ مكتملة في نطاق QEMU الحالي؛ AsasGUI تعمل كـthread مستقل مع event loop كامل، double buffering، window manager تفاعلي، نافذة Terminal تعرض مخرجات Shell الفعلية وسطر الإدخال الحالي، File Manager يقرأ من VFS فعلاً، النوافذ قابلة للسحب والإغلاق، font يغطي كامل ASCII القابل للطباعة، وشريط مهام (Taskbar) مع أزرار toggle للنوافذ.

- ✅ إدارة Framebuffer.
- ✅ رسم الخطوط والأشكال والنوافذ الأساسية.
- ✅ إنشاء Window Manager تفاعلي: النوافذ قابلة للسحب بالفأرة وللإغلاق عبر زر X.
- ✅ دعم الفأرة بصريًا عبر مؤشر مرسوم بمخطط سهم مع ظل، مع معالجة أحداث النقر والسحب.
- ✅ Terminal window تفاعلية: تعرض مخرجات Shell الفعلية (عبر logger hook) وسطر الإدخال الحالي المحدَّث لحظةً بلحظة.
- ✅ File Manager window: تقرأ قائمة الملفات من VFS فعلاً عبر vfs_list_directory() وتعيد تحميلها دوريًا.
- ✅ Double buffering كامل: رسم في back buffer ثم blit إلى الشاشة لتجنب التمزق البصري.
- ✅ Event loop مستقل كـkernel thread يُجدوَل عبر preemptive scheduler.
- ✅ Taskbar مع أزرار toggle لكل نافذة (إخفاء/إظهار).
- ✅ Font يغطي كامل ASCII القابل للطباعة (حروف، أرقام، 40+ رمز خاص).

C++ مناسبة لتنظيم النوافذ والعناصر الرسومية والتطبيقات.

## المرحلة 12: الصوت والطاقة والأجهزة المحمولة

**اللغة الأساسية:** C

**الحالة الحالية:** ✅ مكتملة في نطاق QEMU والتوثيق الحالي؛ ACPI FADT/DSDT يُقرأان لاكتشاف PM1 control و`_S5`/`_S3`، وأوامر `power` و`shutdown` و`reboot` و`sleep` موجودة في Shell، ويوجد كشف ACPI مبكر للبطارية والـTouchpad، وكشف PCI مبكر للـWi-Fi، ودعم صوت أولي عبر PC speaker.

- ✅ إدارة الطاقة عبر ACPI.
- ✅ إيقاف التشغيل، وإعادة التشغيل، والنوم كأوامر Shell متاحة؛ اختبار QEMU يتحقق من توفرها دون تنفيذها.
- ✅ دعم بطارية Laptop: كشف ACPI namespace للبطارية عند توفر `PNP0C0A` أو `_BIF`/`_BST`، مع توثيق انتظار الهاردوير الفعلي في قائمة الأجهزة.
- ✅ دعم الصوت: تهيئة PC speaker وإضافة أمر Shell `beep` متحقق منه عبر QEMU serial.
- ✅ دعم Touchpad: كشف ACPI namespace لأجهزة Touchpad الشائعة، مع توثيق انتظار الهاردوير الفعلي في قائمة الأجهزة.
- ✅ دعم Wi-Fi: كشف PCI class لأجهزة Wi-Fi/Network Controller غير Ethernet، مع توثيق انتظار الهاردوير الفعلي في قائمة الأجهزة.

دعم Wi-Fi وبطاقات الرسومات الحديثة معقد جداً؛ لذلك يبدأ المشروع بدعم أجهزة
محددة بدلاً من محاولة دعم جميع الأجهزة.

## المرحلة 13: الأمان والاستقرار

**اللغات:** C وC++، مع أدوات Python

**الحالة الحالية:** ✅ مكتملة ومتحققة في نطاق QEMU الحالي؛ بعض العناصر طُبِّقت مبكراً كجزء من تحسينات معمارية، ونموذج المستخدمين والصلاحيات وASLR للـUser Stack واختبارات الضغط/التسريب وCrash Log تعمل ومتحققة عبر QEMU.

- ✅ فصل النواة عن برامج المستخدم (مكتمل منذ المرحلة 6).
- ✅ التحقق من جميع مدخلات System Calls (Pointer Validation مُطبَّق).
- ✅ منع تنفيذ صفحات البيانات (NX bit مُفعَّل منذ المرحلة 4).
- ✅ SMEP/SMAP لحماية النواة من User Space (مُطبَّق).
- ✅ Double Fault IST1 Emergency Stack لمنع Triple Fault (مُطبَّق).
- ✅ Atomic SMP counter لمنع Race Condition عند إقلاع الأنوية (مُطبَّق).
- ✅ إضافة المستخدمين والصلاحيات: مستخدم `root` ومستخدم `guest`، وصلاحيات قراءة/كتابة/تنفيذ/إدارة، وأوامر `whoami` و`permissions`، وفحوصات على أوامر الملفات والطاقة والإدارة.
- ✅ دعم ASLR: seed مبكر وتوزيع عشوائي لعنوان User Stack عند تحميل برنامج المستخدم؛ نقل Image Base لبرامج PE القابلة لإعادة التمركز يبقى امتداداً لاحقاً إذا احتجناه.
- ✅ إجراء اختبارات ضغط وتسريب ذاكرة: فحص ضغط Heap مع مقارنة المساحة الحرة قبل/بعد، وفحص frame allocator مع `frame_free` للتأكد من عدم تسريب صفحات.
- ✅ تشغيل الاختبارات تلقائياً داخل QEMU: السكربت `tests/Run-All.ps1` يبني صورة الإقلاع ثم يشغل اختبار QEMU الكامل ويتحقق من رسائل Serial والـHTTP host probe.
- ✅ تسجيل الأعطال وتحليلها: Crash Log ring buffer داخل النواة، و`panic()` يسجل الحدث قبل التوقف، وSelf-test يتحقق من تسجيل وتحليل آخر حدث.

## خطة الإصدارات

### الإصدار 0.1

**الحالة الحالية:** ✅ مكتمل

- ✅ إقلاع UEFI.
- ✅ شاشة وSerial.
- ✅ إدارة الذاكرة.
- ✅ Keyboard (PS/2).
- ✅ Shell داخل النواة.

### الإصدار 0.2

**الحالة الحالية:** ✅ مكتمل

- ✅ عمليات وUser Mode وRing 3.
- ✅ System Calls (INT 0x80).
- ✅ تشغيل برامج PE64 (تم اختيار PE64 بدلاً من ELF64 لأن بيئة البناء Windows/MSVC).
- ✅ FAT16 قراءة وكتابة ومجلدات فرعية (FAT32 مؤجل للإصدارات اللاحقة).
- ✅ VFS مدمج مع محرك الأقراص الفعلي.
- ✅ SDK وبرامج C وC++ مستقلة.

### الإصدار 0.3

**الحالة الحالية:** ✅ مكتمل في نطاق QEMU الأساسي، مع بقاء التحقق على أجهزة فعلية خارج نطاق QEMU الحالي

- ✅ Shell وبرامج مستخدم حقيقية (مكتمل مبكراً).
- ✅ USB عبر xHCI — HID Keyboard وMouse يعملان؛ USB Mass Storage يتم تعداده وتكوين Bulk endpoints له ويقرأ sector عبر BOT/SCSI.
- ✅ AHCI قراءة كاملة: تركيب قرص SATA في QEMU وقراءة sector عبر AHCI READ DMA EXT متحققان آلياً.
- ✅ NVMe: تركيب قرص NVMe في QEMU، تهيئة Admin/I/O queues، وقراءة sector من namespace 1 متحققة آلياً.
- ✅ دعم أجهزة PC فعلية محددة: قائمة الأجهزة المستهدفة موثقة في `HARDWARE_COMPATIBILITY.md`، والتحقق الفيزيائي يُنفذ على الأجهزة المذكورة عند توفرها.

### الإصدار 0.4

**الحالة الحالية:** ✅ مكتمل في نطاق QEMU الحالي

- ✅ شبكة TCP/IP (VirtIO Network أولاً ثم بطاقات Intel): Ethernet/ARP/IPv4/ICMP وDHCP وDNS query وTCP/HTTP الأساسي و`wget` مع حفظ الملف و`http-server` تعمل على QEMU.
- ✅ مستخدمون وصلاحيات.
- ✅ FAT32 أو نظام ملفات موسّع: FAT32 قراءة وكتابة وحذف ملفات ومجلدات مدمج في VFS، وNTFS قراءة فقط مدمج في VFS.
- ✅ ASLR للـUser Stack.

### الإصدار 1.0

**الحالة الحالية:** ✅ مكتمل في نطاق QEMU والتوثيق الحالي

- ✅ واجهة رسومية أساسية (AsasGUI).
- ✅ استقرار جيد واختبارات ضغط في نطاق QEMU الحالي.
- ✅ مثبت للنظام: `tools/Install-Asas.ps1`.
- ✅ دعم قائمة موثقة من أجهزة Laptop وPC: `HARDWARE_COMPATIBILITY.md`.

## استراتيجية دعم الأجهزة

سيبدأ التطوير والاختبار على:

1. QEMU كبيئة تطوير واختبار أساسية.
2. جهاز Laptop أو PC حقيقي واحد بمواصفات محددة.
3. إضافة أجهزة أخرى تدريجياً بعد استقرار التعريفات الأساسية.

محاولة دعم جميع أجهزة Laptop وPC منذ البداية غير واقعية بسبب العدد الكبير
والتنوع الشديد في تعريفات الأجهزة.
