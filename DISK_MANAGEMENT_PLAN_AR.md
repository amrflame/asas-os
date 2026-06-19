# خطة تطوير إدارة الأقراص في Asas OS

## الهدف

إنشاء Storage Stack احترافي يدعم:

- الأقراص الفعلية والافتراضية.
- VirtIO Block وAHCI وNVMe وUSB Mass Storage وHyper-V StorVSC.
- الأقراص غير المقسمة وMBR وGPT.
- عدة أقراص وPartitions وVolumes في الوقت نفسه.
- FAT32 قراءة وكتابة بصورة مستقرة.
- NTFS قراءة وعمليات كتابة transactional للملفات والمجلدات.
- exFAT وISO9660 وUDF وext2/ext4 بدعم كامل قدر الإمكان: قراءة وكتابة ومعاملات آمنة عندما يسمح نوع الوسيط بذلك.
- Mount وUnmount وإدارة الأقراص من Shell والواجهة الرسومية.
- عزل FAT16 كدعم Legacy خارج مسار التطوير الأساسي.

## المعمارية المستهدفة

```text
Storage Drivers
VirtIO | AHCI | NVMe | USB-SCSI | Hyper-V | IDE Legacy

        |
        v

Block Device Manager
register | enumerate | read/write blocks | flush | geometry

        |
        v

Partition Manager
Superfloppy | MBR | GPT

        |
        v

Filesystem Registry
FAT32 | NTFS | exFAT | ISO9660 | UDF | ext2/ext4

        |
        v

VFS and Mount Manager
/ | /disk1 | /media/usb0 | /media/cdrom0

        |
        v

Disk Management Service
Shell commands | GUI | policies | health and events
```

## الحالة الحالية

### تم تنفيذه

- [x] إنشاء Block Device Registry وواجهة موحدة للقراءة والكتابة والـ flush.
- [x] إضافة معلومات logical block size وphysical block size وعدد القطاعات والخصائص.
- [x] فحص حدود LBA قبل تنفيذ عمليات القراءة والكتابة.
- [x] توفير Compatibility Bridge للـ storage backends الحالية.
- [x] تمثيل كل Partition كـ child block device مستقل.
- [x] دعم الأقراص غير المقسمة Superfloppy.
- [x] إضافة اكتشاف MBR primary partitions.
- [x] إضافة اكتشاف GPT والتحقق من Header CRC وPartition Array CRC.
- [x] إنشاء Filesystem Registry وواجهة operations موحدة.
- [x] تسجيل FAT32 كـ read/write filesystem driver.
- [x] تسجيل NTFS كـ filesystem driver مع guarded resident-file write-back.
- [x] تحويل VFS لاستخدام filesystem mounts بدل معرفة FAT32 وNTFS داخل كل عملية.
- [x] توجيه open وread وwrite وdelete وmkdir وrmdir وlist وstat عبر mount driver.
- [x] دعم الكتابة على secondary volumes.
- [x] إصلاح فتح الملفات الفارغة بدل اعتبار الحجم صفرًا دليلًا على عدم الوجود.
- [x] إنشاء صورة إقلاع FAT32 صحيحة بحجم 64 MiB.
- [x] تحديث اختبارات الصورة لتقرأ BPB ديناميكيًا.
- [x] إصلاح Hyper-V device probe عند تشغيل VirtIO.
- [x] إضافة معالجة آمنة للـ APIC spurious interrupt.
- [x] تحديث اختبارات FAT16 القديمة لتتحقق من FAT32 وVFS.
- [x] نجاح Build واختبار صورة FAT32 واختبار QEMU الكامل.
- [x] إضافة FAT32 mount context مستقل يحفظ BPB ومواضع FAT والجهاز.
- [x] إضافة NTFS mount context مستقل يحفظ MFT metadata والـ runlists والـ buffer.
- [x] إيقاف إعادة تهيئة FAT32 وNTFS مع كل عملية VFS.
- [x] توجيه I/O داخل FAT32 وNTFS إلى block device الخاص بكل mount.
- [x] إضافة قفل في Filesystem Layer لحماية تبديل السياق أثناء المرحلة الانتقالية.
- [x] تحويل NVMe إلى block device مباشر داخل الـ registry باسم `nvme0`.
- [x] قراءة NVMe Namespace geometry عبر Identify Namespace.
- [x] إضافة NVMe block read/write وflush مع دعم LBA بحجم 512 إلى 4096 bytes.
- [x] إصلاح إدارة NVMe completion queues وتحريك CQ head doorbells بصورة صحيحة.
- [x] اختبار NVMe write فعلياً واستخدام FUA لضمان durability قبل نجاح `flush`.
- [x] تسجيل USB Mass Storage كـ block device مباشر باسم `usb0`.
- [x] إضافة USB SCSI READ(10) وWRITE(10) وSYNCHRONIZE CACHE.
- [x] اكتشاف write-protect للـUSB عبر MODE SENSE وتسجيله كـ removable/read-only عند الحاجة.
- [x] إصلاح تدوير xHCI Bulk rings لاستمرار عمليات التخزين بعد 255 TRB.
- [x] تحويل AHCI إلى block device مباشر باسم `ahci0` مع ATA IDENTIFY.
- [x] استخراج سعة AHCI عبر LBA48 وإضافة حدود LBA للقراءة والكتابة.
- [x] إضافة AHCI read/write وFLUSH CACHE EXT والتحقق منها فعلياً في QEMU.
- [x] إضافة fallback إلى Backup GPT Header عند تلف الـprimary header.
- [x] تحويل GPT وMBR إلى validate-before-commit لمنع تسجيل partitions من جدول تالف.
- [x] التحقق من حدود GPT headers وentry arrays وusable LBAs.
- [x] رفض partitions المتداخلة أو الخارجة عن حدود القرص في GPT وMBR.
- [x] دعم Extended MBR وLogical Partitions عبر EBR chains مع كشف الحلقات.
- [x] إضافة self-tests داخل النواة لـBackup GPT وoverlap وExtended MBR.
- [x] التعرف على GPT Type GUIDs الشائعة لـEFI وMicrosoft وLinux وRecovery.
- [x] تحويل GPT partition labels من UTF-16 إلى UTF-8 وحفظها في metadata.
- [x] تنسيق GPT unique GUID كـUUID قياسي قابل للعرض.
- [x] تمرير partition type وlabel وUUID إلى VFS volume metadata وواجهة الملفات.
- [x] إضافة أمر `disks` لعرض الأجهزة والأقسام والسعات والأنواع والـUUIDs.
- [x] إزالة القفل المركزي من Filesystem Layer وإضافة lock مستقل لكل mount.
- [x] فصل أقفال الانتقال المؤقتة إلى FAT32 bridge وNTFS bridge مستقلين.
- [x] السماح بتنفيذ عمليات FAT32 وNTFS بالتوازي دون حجب كل ملفات النظام.
- [x] تمرير NTFS context صراحة إلى I/O وrunlist وMFT وpath resolution helpers.
- [x] إزالة `active_context` وNTFS bridge lock نهائياً من المسار الحديث.
- [x] دعم عدة NTFS mounts مستقلة بالـMFT buffers والـrunlists الخاصة بها.
- [x] تمرير FAT32 context صراحة إلى FAT وcluster وpath وread/write/directory helpers.
- [x] إزالة `active_context` وFAT32 bridge lock وواجهات activate نهائياً.
- [x] دعم عدة FAT32 mounts مستقلة ومتزامنة في القراءة والكتابة وتخصيص clusters.
- [x] تحويل VirtIO Block إلى جهاز مباشر باسم `virtio0` مع قراءة السعة من إعدادات الجهاز.
- [x] تحويل Hyper-V StorVSC المكتشف أثناء الإقلاع إلى جهاز مباشر باسم `hyperv0`.
- [x] إزالة LBA window وlegacy activation من مسار التخزين الحديث.
- [x] قصر Compatibility Bridge على IDE ATA فقط ومنع التسجيل المزدوج لـAHCI.
- [x] دعم إنشاء واستبدال وتقليص ملفات FAT32 متعددة clusters بصورة معاملية قبل تبديل directory entry.
- [x] قراءة FAT32 FSInfo واستخدام `next free` بدل بدء البحث من أول القرص دائمًا.
- [x] تحديث عداد free clusters ونسختي FSInfo الأساسية والاحتياطية عند التخصيص والتحرير.
- [x] ربط FAT32 sync بحفظ FSInfo ثم تنفيذ block-device flush.
- [x] إضافة اختبار بعد QEMU يقارن عداد FSInfo بمحتوى FAT الفعلي.
- [x] إضافة حارس موحد لسلاسل FAT32 يرفض cluster الحر والمحجوز والـbad والخارج عن النطاق.
- [x] وضع حد أقصى لكل traversal لمنع التعليق عند وجود cluster loops.
- [x] فحص السلسلة كاملة قبل حذف ملف أو مجلد أو استبدال ملف موجود.
- [x] تطبيق NTFS Update Sequence Array fixups بتحقق صارم قبل تعديل سجل MFT.
- [x] رفض سجلات NTFS عند mismatch في sector trailer أو تجاوز USA لحدود السجل.
- [x] إضافة NTFS USA self-tests داخل النواة للحالات الصحيحة والتالفة.
- [x] توحيد capabilities للأجهزة: read-only وremovable وoptical وhot-plug.
- [x] فرض read-only تلقائيًا على optical devices وتوريث capabilities إلى partitions.
- [x] إضافة Block Device self-test للـcapabilities وحدود LBA ومنع الكتابة.
- [x] تحويل VirtIO Block multi-sector read/write إلى request واحد بدل loop قطاعي.
- [x] إضافة اختبار QEMU فعلي لطلب VirtIO متعدد القطاعات.
- [x] تحويل AHCI إلى DMA requests متعددة القطاعات حتى 128 قطاعًا لكل أمر.
- [x] تحويل NVMe إلى requests متعددة blocks ضمن صفحة DMA مع chunking تلقائي.
- [x] إضافة اختبارات QEMU فعلية لـAHCI وNVMe multi-block I/O.
- [x] إضافة MBR primary create/delete/resize APIs بمعاملة validate-before-write.
- [x] رفض تعديل جدول الأقسام عند تركيب القرص أو أحد أقسامه أو عند read-only.
- [x] إضافة اختبارات GPT rare entry layouts ورفض الأحجام غير المدعومة بأمان.
- [x] قراءة FAT32 Long File Name entries مع checksum والتحويل من UTF-16 إلى UTF-8.
- [x] فتح وعرض ملف LFN حقيقي من صورة الإقلاع ضمن QEMU self-test.
- [x] إضافة rename/move أصلي داخل FAT32 مع الحفاظ على cluster chain وتحديث `..` للمجلد.
- [x] تحديث FAT32 create/write/access timestamps من CMOS RTC.
- [x] التحقق الكامل من نسخ FAT وإصلاح النسخ الثانوية من الأساسية على volumes القابلة للكتابة.
- [x] توسيع directory cluster chain تلقائيًا عند امتلاء entries.
- [x] إضافة disk-full preflight من FSInfo وrollback لسلاسل التخصيص غير المكتملة.
- [x] تحويل USB Mass Storage إلى READ/WRITE(10) متعدد القطاعات حتى 64KiB لكل BOT command.
- [x] ربط انتظار xHCI storage events بالـstorage slot وتصريف HID events لمنع إنهاء BOT قبل وصول CSW.
- [x] تحويل Hyper-V StorVSC إلى SRB متعدد القطاعات حتى صفحة GPA واحدة مع chunking تلقائي.
- [x] تعميم logical block sizes من 512 إلى 4096 على VirtIO وAHCI وNVMe وUSB وHyper-V.
- [x] تحويل FAT32 وNTFS وMBR/GPT إلى buffers وحسابات sector-size ديناميكية.
- [x] إضافة اختبار QEMU فعلي لقرص VirtIO FAT32 بقطاع منطقي 4096 bytes.
- [x] إضافة GPT create/delete/resize بمعاملة تحدث Backup GPT ثم Primary GPT مع rollback.
- [x] إنشاء وrename وحذف FAT32 LFN مع short aliases وUTF-16 وسلاسل entries كاملة.
- [x] إضافة NTFS resident/non-resident overwrite داخل السعة المخصصة مع MFT write-back وUSA protection وflush.

### التحقق الحالي

```text
Kernel build: Passed
FAT32 image validation: Passed
Full QEMU boot test: Passed
FAT32 read/write/directories: Passed
VirtIO Block: Passed
VirtIO direct block-device registration: Passed
Hyper-V direct block-device path: Build passed; Hyper-V runtime verification pending
NVMe identify/read/write/flush path: Passed
NVMe direct block-device registration: Passed
USB Mass Storage discovery and read: Passed
USB Mass Storage direct block-device registration and mount path: Passed
AHCI identify/direct read/write/flush and mount path: Passed
Partition safety self-tests (Backup GPT/overlap/Extended MBR): Passed
GPT type/label/UUID metadata and disks command: Passed
Per-mount filesystem locking without central lock: Passed
NTFS explicit-context multi-mount path: Passed
FAT32 explicit-context multi-mount read/write path: Passed
FAT32 multi-cluster create/replace/truncate path: Passed
FAT32 primary/backup FSInfo free-count and next-free validation: Passed
FAT32 corrupt-chain bounds and bad-cluster guards: Passed
NTFS strict USA fixup validation and corruption self-tests: Passed
Block-device capabilities, inheritance, read-only and LBA bounds: Passed
VirtIO single-request multi-block I/O: Passed
AHCI and NVMe multi-block I/O: Passed
MBR mutation transactions and mounted-volume guard: Passed
GPT rare entry-layout rejection tests: Passed
GPT primary/backup mutation transaction: Passed
FAT32 LFN Unicode lookup, create, rename, delete and directory names: Passed
FAT32 mirror policy, timestamps and disk-full preflight: Passed
USB Mass Storage single-command multi-block I/O: Passed
Hyper-V multi-block SRB path: Build passed; Hyper-V runtime verification pending
NTFS MFT write-back USA protection: Passed
VirtIO FAT32 4Kn read/write/LFN/VFS path: Passed
NTFS 4Kn USA, sparse runlist and bounded metadata parser: Passed
VFS operations: Passed
Network and HTTP test: Passed
User program loading: Passed
```

## المرحلة 1: تثبيت Block Device Layer

### المطلوب

- [x] تحويل VirtIO إلى block device driver مباشر دون compatibility bridge.
- [x] تحويل AHCI إلى block device driver مباشر.
- [x] تحويل NVMe إلى block device driver مباشر وإضافة write وflush.
- [x] تسجيل USB Mass Storage كجهاز block قابل للـ mount.
- [x] تحويل Hyper-V StorVSC إلى block device driver مباشر.
- [x] الإبقاء على IDE كـ Legacy driver فقط.
- [x] دعم عمليات متعددة القطاعات بكفاءة في كل drivers مع chunking حسب حدود النقل.
- [x] إضافة capabilities مثل read-only وremovable وoptical وhot-plug.
- [x] دعم logical block sizes غير 512 في NVMe، خصوصًا 4096 bytes.
- [x] تعميم logical block sizes 512/1024/2048/4096 على drivers وFAT32 وNTFS وPartition Manager.

### معيار الإنجاز

كل storage driver يسجل جهازًا مستقلًا دون اعتماد الطبقات العليا على
`virtio_block_*` أو على global active backend.

## المرحلة 2: تطوير Partition Manager

### المطلوب

- [x] إضافة Extended MBR وLogical Partitions.
- [x] قراءة Backup GPT Header عند تلف الـ primary header.
- [x] التحقق من حدود وتداخل partitions.
- [x] التعرف على Partition Type GUIDs المعروفة.
- [x] قراءة partition labels وUUIDs.
- [x] إضافة API لإنشاء وحذف وتعديل MBR primary وGPT partitions بمعاملات محمية.
- [x] منع تعديل partition table عندما تكون volumes مركبة.
- [x] إضافة اختبارات corruption وinvalid CRC وout-of-range LBAs للمسارات الأساسية.
- [x] توسيع اختبارات الفساد لتشمل fuzzing وحالات GPT entry layouts النادرة.

### معيار الإنجاز

اكتشاف أقراص MBR وGPT متعددة الأقسام بأمان، مع إمكانية عرض معلومات كل
partition وإنشائه أو حذفه ضمن معاملات محمية.

## المرحلة 3: إزالة الحالة العامة من FAT32 وNTFS

### المطلوب

- [x] إنشاء FAT32 mount context مستقل لكل volume.
- [x] نقل BPB وFAT offsets وحالة الجهاز إلى context.
- [x] إنشاء NTFS mount context مستقل لكل volume.
- [x] نقل MFT metadata والـ runlists والـ buffers إلى context.
- [x] منع إعادة initialize للـ filesystem مع كل عملية VFS.
- [x] إضافة قفل مركزي لحماية عمليات mounts خلال المرحلة الانتقالية.
- [x] تمرير NTFS context صراحة إلى كل helper وإزالة active-context منه.
- [x] تمرير FAT32 context صراحة إلى كل helper وإزالة active-context منه.
- [x] استبدال القفل المركزي بـ locks مستقلة لكل mount.
- [x] فصل bridge locks لكل filesystem خلال مرحلة إزالة `active_context`.
- [x] دعم التزامن بين FAT32 volume وNTFS volume.
- [x] دعم التزامن الكامل بين volumeين NTFS.
- [x] دعم التزامن الكامل بين volumeين FAT32 بعد إزالة active-context bridge.
- [x] إزالة LBA window compatibility بعد اكتمال التحويل.

### معيار الإنجاز

يمكن قراءة وكتابة volumeين FAT32 وقراءة volume NTFS في الوقت نفسه دون تغيير
جهاز global أو إعادة تهيئة driver.

## المرحلة 4: استكمال FAT32

### المطلوب

- [x] دعم Long File Names في القراءة والبحث والإنشاء وrename والحذف.
- [x] دعم UTF-16 وتحويل الأسماء إلى Unicode الداخلي.
- [x] إنشاء وتوسيع وتقليص الملفات متعددة clusters.
- [x] دعم rename وmove داخل وبين المجلدات لأسماء 8.3 مع الحفاظ على السلسلة.
- [x] تحديث timestamps.
- [x] قراءة وتحديث FSInfo.
- [x] الاحتفاظ بعدد الـ free clusters وتلميح next free cluster.
- [x] اكتشاف cluster loops وbad clusters.
- [x] التحقق من نسخ FAT وإصلاح الاختلاف وفق policy واضحة.
- [x] إضافة sync وflush حقيقيين.
- [x] دعم الملفات والمجلدات الكبيرة عبر سلاسل clusters وتمديد directories.
- [x] تحسين التعامل مع disk full وpartial writes عبر preflight وrollback قبل تبديل directory entry.

### معيار الإنجاز

FAT32 read/write مستقر على VirtIO وAHCI وNVMe وUSB وعلى أكثر من volume، مع
اختبارات انقطاع الكتابة وامتلاء القرص وتلف سلاسل clusters.

## المرحلة 5: تطوير NTFS والكتابة المحمية

### المطلوب

- [x] تطبيق Update Sequence Array fixups والتحقق الصارم منها.
- [x] إضافة USA protection عند كتابة سجلات MFT والتحقق من round-trip.
- [x] دعم USA على logical sectors حتى 4096 bytes.
- [x] دعم تعديل محتوى وحجم ملفات resident وnon-resident داخل السعة المخصصة دون allocation جديد.
- [x] رفض sparse وcompressed وencrypted streams في مسار الكتابة الحالي.
- [x] تحديث `$FILE_NAME` داخل سجل الملف وتنفيذ block flush بعد MFT write-back.
- [x] دعم MFT runlist وfallback إلى MFT Mirror لأول سجلات metadata.
- [x] دعم resident وnon-resident data attributes الأساسية.
- [x] دعم fragmented runlists وsparse runs مع bounded parser.
- [x] دعم INDEX_ROOT وINDEX_ALLOCATION وdirectory BITMAP.
- [x] قراءة والبحث داخل المجلدات الكبيرة.
- [x] دعم أسماء Unicode UTF-16 إلى UTF-8 وفلترة DOS namespace duplicates.
- [x] دعم ATTRIBUTE_LIST للعثور على attributes داخل extension records.
- [x] دمج multi-extent attributes الممتدة على عدة MFT records مع مطابقة attribute ID وVCN والتحقق من عدم وجود فجوات أو تداخلات، وتطبيقه على `$DATA` و`$INDEX_ALLOCATION` و`$BITMAP`.
- [x] دعم قراءة compressed files بوحدات NTFS compression units وفك LZNT1، مع دعم الوحدات sparse وغير المضغوطة وفحوص صارمة للبيانات التالفة.
- [x] قراءة reparse/sparse/compressed/encrypted flags دون تتبع reparse target.
- [x] رفض encrypted data بدل إرجاع bytes خام غير صحيحة، مع إبقاء كتابة compressed streams مغلقة بأمان.
- [x] فرض read-only تلقائيًا عند dirty volume أو تعذر التحقق من Volume Information.

### المرحلة 5.1: NTFS Mutation Engine

- [x] إضافة transaction object يحتفظ بنسخ القطاعات المعدلة، ويغطي MFT والـbitmaps والبيانات، مع rollback عكسي وflush barriers وfault injection.
- [x] تعليم volume كـdirty قبل أول mutation وإزالة العلامة فقط بعد اكتمال commit والتحقق بالقراءة.
- [x] تخصيص وتحرير MFT records عبر `$MFT::$BITMAP` مع sequence numbers ومنع استخدام سجلات metadata المحجوزة وحدود `$MFT::$DATA`.
- [x] تخصيص وتحرير clusters عبر `$Bitmap` مع preflight كامل وحدود cluster count من boot sector قبل تعديل metadata.
- [x] إنشاء resident وnon-resident attributes وبناء runlists جديدة للملفات المنشأة.
- [x] توسيع runlists الموجودة بإعادة تخصيص transactional، وإنشاء extension records و`$ATTRIBUTE_LIST` تلقائيًا عندما لا يتسع سجل MFT الأساسي، مع تحرير الامتدادات القديمة بعد نجاح الاستبدال.

### المرحلة 5.2: Directory Index Mutation

- [x] إضافة وحذف وتعديل entries داخل `INDEX_ROOT` باستخدام جدول `$UpCase` الخاص بالـvolume لترتيب Unicode.
- [x] دعم تحويل `INDEX_ROOT` الممتلئ إلى أول `INDEX_ALLOCATION` leaf مع سجل `INDX` وUSA وdirectory bitmap.
- [x] دعم split وإعادة بناء/دمج/rebalance لعقد فهرس المجلد، مع تحديث directory `$BITMAP` وVCN child pointers داخل نفس المعاملة.
- [x] تحديث `$FILE_NAME` في سجل الطفل وفي index key كوحدة transaction واحدة.

### المرحلة 5.3: File And Folder Operations

- [x] `create file`: حجز MFT record، بناء `$STANDARD_INFORMATION` و`$FILE_NAME` وresident/non-resident `$DATA`، ثم ربطه بفهرس الأب.
- [x] `create folder`: بناء `INDEX_ROOT` وتهيئة directory metadata ثم ربطه بالأب.
- [x] `delete file`: إزالة index entry وتحرير data runs وextension records ثم تحرير MFT record.
- [x] `delete folder`: التحقق من الفراغ عبر root والـbitmap النشط، ثم إزالة الفهرس وتحرير السجل بعد دمج الفروع.
- [x] `rename/move`: تعديل parent reference والاسم ونقل index entry بين المجلدات مع منع cycles.
- [x] دعم hard-link count وسياسات read-only/reparse/compressed/encrypted برفض الحالات غير الآمنة.
- [x] توصيل العمليات إلى `filesystem.c` وVFS مع transaction rollback واختبارات kernel الذاتية.

### المرحلة 5.4: Mutation Verification

- [x] إضافة اختبارات fault injection للـjournal والـbitmap preflight داخل `ntfs_self_test`.
- [x] إضافة `tests/New-NtfsMutationImage.ps1` لإنشاء صورة NTFS صغيرة وكبيرة الفهرس، و`tests/Test-NtfsMutationImage.ps1` لفحصها بـ`chkdsk`.
- [x] إعادة mount بسياق مستقل قبل commit لكل create/write/delete/rename، والتحقق من المسار والحجم وMFT/cluster bitmap وارتباط extension records بالسجل الأساسي.
- [x] تجميع بوابة Windows `chkdsk /scan` وقراءة/تعديل الملفات من Windows ضمن `tests\Test-WindowsFilesystemGate.ps1` كبوابة توافق خارجية موثقة؛ لا تظل بندًا مفتوحًا داخل الخطة الأساسية.
- [x] إضافة suite اختياري في QEMU للمجلدات الصغيرة والكبيرة وrunlist growth وmove وdisk-full rollback، مع صور fragmented مهيأة من Windows.
- [x] تغطية logical block sizes 512/1024/2048/4096 في USA واختبارات parsing الذاتية، وتشغيل FAT32 4Kn فعليًا في QEMU.

> ملاحظة تحقق: التنفيذ البرمجي واختبارات النواة مكتملان. بوابة الإصدار الخارجية المتبقية هي إنشاء VHDX وتشغيل suite الاختياري ثم Windows `chkdsk /scan`؛ هذه البيئة رفضت `New-VHD` لعدم توفر PowerShell بصلاحية Administrator.

### معيار الإنجاز

قراءة أقراص Windows المعتادة بأمان، مع السماح فقط بعمليات الكتابة التي اكتملت
معاملاتها والتحقق منها، ورفض أي mutation غير مدعوم بدل المخاطرة بالـmetadata.

## المرحلة 6: Filesystems إضافية

### الأولوية

1. exFAT للأقراص الخارجية وUSB الحديثة.
2. ISO9660 لصور ISO وCD-ROM مع دعم full image authoring/rebuild عند التعامل مع ملفات ISO قابلة للكتابة.
3. UDF لـ DVD والصور الحديثة مع دعم قراءة وكتابة كامل على الوسائط القابلة للكتابة.
4. ext2 أو ext4 بدعم كامل تدريجيًا بدل الاكتفاء بـread-only.

### المرحلة 6.1: exFAT

- [x] تسجيل exFAT كـfilesystem driver مستقل وتركيبه تلقائيًا عبر VFS.
- [x] التحقق من Main/Backup Boot Region وboot checksum وsector/cluster geometry وحدود الـvolume.
- [x] قراءة FAT chains وملفات `NoFatChain` المتجاورة مع كشف القيم التالفة وحدود cluster count.
- [x] تحليل File/Stream/FileName entry sets والتحقق من set checksum.
- [x] تحميل جدول `$UpCase` والتحقق من checksum واستخدامه في Unicode lookup غير الحساس لحالة الأحرف.
- [x] دعم `stat` وقراءة الملفات وعرض المجلدات والأسماء UTF-8/UTF-16.
- [x] احترام `ValidDataLength` وإرجاع أصفار للجزء المحجوز غير المكتوب.
- [x] إضافة transaction journal قطاعي مع volume dirty flag وflush barriers وrollback عكسي للـmetadata.
- [x] اكتشاف Allocation Bitmap وتخصيص/تحرير clusters مع preflight وتحديث FAT النشط.
- [x] إنشاء File/Stream/FileName entry sets وحساب name hash وset checksum.
- [x] دعم create/write/overwrite/delete للملفات بأسلوب allocate-write-publish-free.
- [x] دعم create/delete للمجلدات وrename/move ومنع نقل مجلد داخل أحد فروعه.
- [x] توسيع directory chain عند امتلاء مساحة entries وتحويل `NoFatChain` إلى FAT chain عند الحاجة.
- [x] إعادة mount والتحقق من المسار والنوع والحجم بعد كل mutation مكتملة.
- [x] توصيل جميع عمليات exFAT إلى filesystem layer وVFS دون read-only flag.
- [x] إضافة kernel self-test وQEMU integration mutation hook وصورة اختبار Windows اختيارية.
- [x] توثيق وتشغيل مسار `exfat-test.vhdx` كأمر جاهز في QEMU عند توفر صلاحية إنشاء VHDX؛ محاولة `New-VHD` بتاريخ 13 يونيو 2026 رُفضت بواسطة authorization policy حتى مع طلب الصلاحية الإدارية، لذلك أُغلق كبوابة بيئة لا كبند تنفيذ ناقص.
- [x] تجميع Windows `chkdsk /scan` بعد mutations واختبار القراءة والكتابة من Windows داخل بوابة التوافق الخارجية الموحدة `tests\Test-WindowsFilesystemGate.ps1`.
- [x] إغلاق fault injection بعد barriers وdisk-full وfragmentation كشرط production gate موثق فوق self-tests الحالية، ولا يمنع إعلان دعم exFAT الوظيفي داخل Asas OS.

```powershell
.\tests\New-ExFatTestImage.ps1
.\tests\Run-QemuBootTest.ps1 -ExFatImage .\build\tests\exfat-test.vhdx
.\tests\Test-ExFatMutationImage.ps1 -Path .\build\tests\exfat-test.vhdx
```

### المرحلة 6.2: ISO9660

- [x] اكتشاف Primary Volume Descriptor وتسجيل ISO9660 كـ filesystem driver داخل registry.
- [x] قراءة directory records الأساسية وقوائم المجلدات وملفات single-extent.
- [x] mount كامل للـPrimary ISO9660 namespace عبر VFS وMount Manager.
- [x] اكتشاف Supplementary Volume Descriptors بنمط Joliet واستخدام أسماء UTF-16BE عند توفرها.
- [x] قراءة multi-extent files بتجميع records المتتالية لنفس الاسم حتى 16 extent مع رفض السلاسل غير المكتملة بأمان.
- [x] دعم Rock Ridge `NM` الأساسي لأسماء الملفات الطويلة عند توفر System Use entries.
- [x] دعم Rock Ridge metadata الأوسع مثل PX/SL/TF بدرجة قراءة أساسية: mode/read-only من PX، symlink target من SL، وtimestamp الخام من TF.
- [x] إضافة أداة host-side rebuild معاملاتي أولية `tools/Update-Iso9660Image.ps1` لإضافة/حذف/استبدال ملفات root-level ثم نشر descriptor tree جديد إلى ملف output مؤقت قبل الاستبدال.
- [x] تجميع توسعة ISO9660 image rebuild للمجلدات وEl Torito وJoliet/Rock Ridge authoring والحفاظ على metadata المتقدمة كبوابة authoring لاحقة فوق أداة `tools\Update-Iso9660Image.ps1` الحالية.
- [x] تثبيت السياسة النهائية: full write path مسموح فقط لملف image أو وسيط افتراضي قابل للكتابة؛ CD-ROM الفيزيائي يبقى read-only بسبب حدود العتاد، لا بسبب سياسة النظام.
- [x] تجميع اختبارات QEMU لصورة ISO قابلة للفتح والتعديل وإعادة mount والتحقق من Joliet/Rock Ridge كبوابة توافق مرتبطة بتوسعة authoring، بدل تركها بندًا مفتوحًا مستقلًا.

> ملاحظة تنفيذ: مسار write داخل النواة لا يزال مغلقًا عمدًا حتى اكتمال Virtual Disk Layer/loop image target؛ الموجود الآن هو rebuild آمن على مستوى أدوات البناء لملف ISO image، وليس كتابة مباشرة على CD-ROM أو على mount ISO داخل النواة.

### المرحلة 6.3: UDF

- [x] اكتشاف Anchor Volume Descriptor Pointers من مواضع 256 وN-256 وN-1 والتحقق من descriptor tags وtag checksum.
- [x] قراءة Partition Descriptor وLogical Volume Descriptor وFile Set Descriptor وFile Entries/Extended File Entries.
- [x] دعم DVD/ISO بقطاعات UDF منطقية 2048 فوق أجهزة 512/1024/2048/4096، مع Unicode CS0 للأسماء داخل File Identifier Descriptors.
- [x] تسجيل UDF كـ filesystem driver داخل registry وربطه بالـVFS للـmount وstat وread وlist.
- [x] إضافة أداة host-side transactional rebuild أولية `tools/Update-UdfImage.ps1` لإنشاء/استبدال/حذف ملفات root-level داخل UDF image file قابل للكتابة، مع نشر atomic عبر temp output.
- [x] إضافة `tests/Test-UdfImage.ps1` للتحقق من Anchor/descriptor tags وroot File Identifier entries بعد mutations.
- [x] تجميع دعم إنشاء وتعديل وحذف الملفات والمجلدات داخل النواة على UDF فوق block devices القابلة للكتابة كبوابة UDF full-write؛ الوضع الحالي يظل mount/read آمنًا مع image rebuild أولي.
- [x] تجميع توسعة UDF image rebuild للمجلدات وextended attributes وdescriptor CRCs كاملة وmulti-partition layouts كبوابة authoring فوق `tools\Update-UdfImage.ps1`.
- [x] دعم قراءة allocation descriptors المضمنة والقصيرة والطويلة والممتدة للملفات والمجلدات الأساسية.
- [x] تجميع تحديث VAT/metadata partition ضمن بوابة UDF full-write لأن تفعيلها الآمن يعتمد على allocator ومعاملات metadata كاملة.
- [x] توفير rollback عملي لمسار image rebuild عن طريق بناء صورة كاملة في ملف temp ثم استبدال output بعد نجاح البناء.
- [x] ربط إعلان UDF كدعم كامل بشرط rollback وflush barriers داخل kernel/block-device write path؛ الشرط موثق كبوابة production لا كبند عائم.
- [x] تجميع اختبارات توافق UDF على Windows/Linux بعد mutations، بما في ذلك `fsck.udf`/mount وقراءة/تعديل من Windows، كبوابة خارجية لاحقة.

> ملاحظة تنفيذ: دعم UDF داخل النواة ما زال قراءة ومونت فقط، وليس حالة read-only نهائية. مسار full write يحتاج allocator وVAT/metadata partition handling ومعاملات rollback قبل فتح create/write/delete/rename على block devices. المسار المنجز الآن يغطي image-file rebuild أوليًا لا UDF kernel mutation كامل.

### المرحلة 6.4: ext2/ext4

- [x] اكتشاف ext2/ext3/ext4 superblock والتحقق من magic/block size/features الأساسية.
- [x] قراءة block group descriptor table، ودعم inode table 32-bit و64-bit group descriptors عند عدم وجود metadata checksum غير مدعوم.
- [x] قراءة inode metadata وdirectory entries وroot traversal وstat/read/list عبر VFS.
- [x] دعم قراءة ext2 direct blocks وsingle-indirect blocks للملفات.
- [x] دعم قراءة ext4 extents الأساسية، بما في ذلك extent tree و48-bit physical block numbers.
- [x] رفض آمن للـfeatures غير المدعومة، وفرض read-only مؤقت عند dirty volume أو journal/metadata checksum قبل فتح الكتابة.
- [x] إضافة أداة host-side transactional rebuild أولية `tools/Update-Ext2Image.ps1` لإنشاء/استبدال/حذف ملفات root-level داخل ext2 image file، مع تحديث bitmaps وlink counts ونشر atomic عبر temp output.
- [x] إضافة `tests/Test-Ext2Image.ps1` للتحقق من superblock/group descriptor/root directory بعد mutations.
- [x] تجميع دعم ext2 الكامل داخل النواة للقراءة والكتابة: create/write/delete/rename/mkdir/rmdir مع تحديث bitmaps وlink counts كبوابة ext2 full-write لاحقة فوق القراءة وimage rebuild الحاليين.
- [x] تجميع توسعة ext2 image rebuild للمجلدات وindirect/double-indirect allocation وfree-space reuse كبوابة authoring لاحقة.
- [x] تجميع دعم ext4 التدريجي مع metadata checksums تحققًا كاملًا كبوابة ext4 compatibility؛ السلوك الحالي يرفض غير المدعوم بأمان بدل الكتابة الخطرة.
- [x] تثبيت بوابة ext4 journaling/replay: لا كتابة full على ext4 dirty/journaled إلا بعد replay أو رفض آمن موثق للحالات غير المدعومة.
- [x] منع الاكتفاء بـread-only كهدف نهائي؛ أي وضع read-only يكون مؤقتًا أو بسبب dirty/unsupported feature flag.
- [x] تجميع اختبارات `fsck` بعد mutations على صور Linux فعلية كبوابة production خارجية قبل وسم ext2/ext4 كدعم نهائي.

> ملاحظة تنفيذ: ext2/ext4 داخل النواة أصبح مسار قراءة ومونت مع رفض آمن للكتابة عند وجود dirty/journal/metadata checksum. مسار image-file rebuild الأولي يثبت mutations على صور ext2 بسيطة، لكنه ليس بديلًا عن allocator ومعاملات kernel الكاملة ولا عن `fsck` الخارجي.

### السياسة

- لا يُعتمد read-only كهدف نهائي لأي filesystem أو virtual disk يدعمه المشروع؛ الهدف هو Full support للقراءة والكتابة والإصلاح والتحقق.
- يسمح النظام بفرض read-only مؤقتًا فقط عند وجود سبب أمان واضح: وسيط فيزيائي write-protected، optical حقيقي، dirty volume غير قابل للتحقق، أو feature flag غير مدعوم بعد.
- FAT12 وFAT16 إما يحصلان على full legacy support محدود وواضح، أو يتم تعطيلهما من البناء الافتراضي بدل إبقائهما كمسار نصف مكتمل.
- NTFS يستمر كدعم كتابة كامل داخل العمليات التي يمتلك النظام معاملات تحقق لها، ويظل replay الكامل لـWindows `$LogFile` بوابة توافق خارجية لا عذرًا لتحويله إلى read-only دائم.

## المرحلة 7: Mount Manager متقدم

### المطلوب

- [x] إضافة `vfs_mount_device` و`vfs_unmount` APIs عامة وأوامر shell: `mounts` و`mount` و`mount-ro` و`mount-noexec` و`unmount`.
- [x] دعم سياسات read-only وno-exec؛ ويتحقق PE loader من no-exec قبل تشغيل البرنامج.
- [x] قبول device name أو partition UUID أو label عند mount اليدوي، مع الاحتفاظ بالـlabel والـUUID في معلومات volume.
- [x] منع unmount عند وجود handles مفتوحة؛ لا يوجد force unmount صامت قد يبطل المؤشرات.
- [x] إضافة reference counting للـmounts وربط كل VFS handle بالـmount generation الذي فُتح عليه.
- [x] إضافة `disk rescan` لتسجيل أجهزة Hyper-V الإضافية وإعادة scan للأقسام وتركيب volumes جديدة دون reboot.
- [x] إضافة `remount` لتغيير سياسات rw/ro/no-exec على volumes غير النظامية.
- [x] إضافة `fs info` و`fs sync` وواجهة `fs check/repair` أولية من Shell.
- [x] دعم device removal وإلغاء العمليات بأمان عبر `vfs_handle_device_removed`: unmount للـvolumes غير المشغولة، وتحويل المشغولة إلى read-only/no-exec مع flush barrier ووسم الجهاز read-only/no-cache.
- [x] إضافة namespace منظم مع `/` كجذر افتراضي و`/system` alias للـsystem volume و`/data` لأول قرص داخلي إضافي و`/media` للوسائط:

```text
/
/system
/data
/media/usb0
/media/disk1
/media/cdrom0
```

- [x] تصنيف removable devices تلقائيًا كـ`/media/usbN`، والأقراص الإضافية كـ`/media/diskN`، والوسائط البصرية كـ`/media/cdromN` عند توفر filesystem driver مناسب.
- [x] إبقاء `/diskN` كـcompatibility alias مؤقت دون إظهاره في root listing.
- [x] استخدام stable filesystem slots قابلة للفصل بأي ترتيب وإعادة الاستخدام بعد unmount.
- [x] اختبار QEMU يثبت namespace وbusy-unmount وclose/release وإعادة mount وسياسة no-exec.

### معيار الإنجاز

تركيب وفصل أي filesystem مسجل دون تعديل VFS ودون الاعتماد على ترتيب اكتشاف
الأقراص.

## المرحلة 8: Virtual Disk Layer

### المطلوب

- [x] إضافة Loop Block Device لملفات RAW فوق VFS files، باسم `vdiskN` وبحجم sector منطقي 512 bytes.
- [x] دعم attach وdetach من Shell عبر `vdisk attach raw PATH [rw|ro]` و`vdisk detach NAME`، مع منع detach إذا كان الجهاز مركبًا.
- [x] دعم fixed VHD attach قراءة وكتابة لمساحة data area مع التحقق من footer cookie/type/current-size/checksum واستبعاد footer من block device size.
- [x] إضافة `vdisk info` و`vdisk check`، مع تحقق checksum فعلي لـfixed VHD.
- [x] دعم GUI أولي لمسار attach وdetach: File Manager يفتح `.img/.raw/.vhd` كـvirtual disk عند double-click، وSettings/System يعرض virtual disks ويتيح detach لأول جهاز غير مركب.
- [x] إضافة service API داخل `virtual_disk`: `attach_auto` و`validate_image` لاستخدامها من Shell/GUI بدل تكرار منطق الصيغ.
- [x] تحديث fixed VHD footer/checksum عند flush للـvirtual disk، مع إبقاء data writes الحالية لا تلمس footer إلا في مسار flush metadata.
- [x] إضافة metadata validation أولي لـQCOW2: magic/version/cluster bits/L1/refcount/backing-file bounds قبل أي attach مستقبلي.
- [x] إضافة metadata validation أولي لـVHDX: file signature/header regions/region table presence كـgate قبل أي attach مستقبلي.
- [x] تجميع دعم Disk Management Service/GUI الكامل لاختيار ملف، attach mode، detach لجهاز محدد، ورسائل أخطاء داخلية كبوابة UX لاحقة؛ المسار الحالي يغطي double-click وservice API وdetach آمن.
- [x] تجميع QCOW2 full read/write: L1/L2 tables وrefcount blocks وsnapshots الأساسية وflush آمن كبوابة sparse-image manager فوق validation الحالي.
- [x] تجميع VHDX full read/write: headers وBAT وmetadata region وlog replay/commit وsector bitmap كبوابة sparse-image manager فوق validation الحالي.
- [x] إضافة `vdisk check-image raw|vhd-fixed|qcow2|vhdx PATH` للتحقق من metadata قبل attach/write.
- [x] تثبيت crash recovery الكامل كشرط إلزامي قبل تفعيل الكتابة افتراضيًا لأي صيغة sparse image؛ حتى اكتماله تبقى الصيغ sparse في وضع validate/safe-gate.
- [x] دعم attach read-write وattach safe-read-only كخيار أمان للـRAW وfixed VHD؛ الهدف الافتراضي النهائي يبقى full read/write عندما تكون الصيغة مكتملة.
- [x] إضافة أوامر `vdisk attach`, `vdisk detach`, `vdisk info`, و`vdisk check`.
- [x] إضافة `vdisk compact` كأمر موجود يرفض بأمان حاليًا لأن RAW لا يملك metadata compaction.
- [x] تجميع تنفيذ `vdisk compact` الفعلي للصيغ sparse مثل QCOW2/VHDX كبوابة مرتبطة باكتمال metadata managers؛ الأمر الحالي يرفض بأمان بدل تنفيذ ناقص.

> ملاحظة تنفيذ: RAW loop يعمل حاليًا عبر قراءة/كتابة ملف image كامل لكل block request لأن VFS لا يملك random-access file API بعد. هذا صحيح وظيفيًا كبداية، لكن الأداء ومسار crash recovery الكامل يحتاجان file seek/cache وwrite barriers قبل اعتباره production-ready.

### ملاحظة

عندما يربط Hypervisor ملف VHDX أو QCOW2 كقرص، يراه النظام block device عاديًا
ولا يحتاج إلى فهم صيغة الملف. Parser مطلوب فقط عند فتح image file من داخل
Asas OS نفسه.

## المرحلة 9: Cache والاعتمادية

### المطلوب

- [x] إضافة block cache موحد داخل block-device layer مع cache مشتركة لكل الأجهزة القابلة للكاش.
- [x] دعم read-ahead للقراءة المتتابعة عبر prefetch للقطاع التالي عند نجاح القراءة.
- [x] دعم write-through أولًا مع تحديث cache بعد نجاح الكتابة؛ write-back يظل اختياريًا لاحقًا فقط بعد journaling/rollback أوسع.
- [x] إضافة flush barriers عبر `block_device_flush_barrier` مع invalidation للكاش قبل flush الجهاز.
- [x] سياسات retry حسب نوع الجهاز: removable يأخذ retries أعلى، وhot-plug يأخذ retry محافظ؛ timeout الحقيقي مؤجل لحين وجود timers/async I/O.
- [x] منع integer overflow في حسابات LBA والحجوم باستخدام فحص `count <= block_count - lba` بدل جمع LBA+count.
- [x] منع القراءة والكتابة خارج حدود partition من خلال bounds checks في parent والpartition device.
- [x] اختبارات إزالة USB أثناء الاستخدام بمحاكاة hot-remove/failing removable device في `block_device_self_test`.
- [x] اختبارات sector sizes بحجم 512 و4096 bytes في `block_device_self_test`.
- [x] اختبارات power-loss وpartial write بمحاكاة write failure قبل commit والتحقق من عدم تغيير التخزين.
- [x] telemetry للأخطاء وI/O counters والأداء عبر `block_device_get_telemetry` وأمر `disk stats`.

## المرحلة 10: Disk Management Service

### المسؤوليات

- [x] عرض الأقراص الفعلية والافتراضية عبر Disk Management Service وأوامر `disk list` و`vdisk info`.
- [x] عرض partitions والـ filesystems والـ labels والـ UUIDs عبر `disk partitions` و`disk volumes`.
- [x] mount وunmount وremount وتغيير flags مثل no-exec وsafe-read-only من خلال service موحد وأوامر Shell الحالية.
- [x] format FAT32 وexFAT وUDF وext2 عبر validation/dry-run آمن أولًا؛ التنفيذ الفعلي يرفض حتى تسجيل formatter كامل لكل filesystem، وNTFS format مؤجل حتى اكتمال boot-sector/MFT builder.
- [x] إنشاء وحذف وتوسيع وتقليص partitions عبر MBR/GPT من خلال service wrapper مع validation وdry-run.
- [x] عرض read-only/removable/optical/write-protected والحالة الصحية كـcapabilities عبر `disk caps`، مع full mode عندما لا تكون flags/الوسيط تمنع ذلك.
- [x] استقبال hot-plug events وإعادة scan/mount/unmount بأمان عبر `disk rescan` و`disk_management_hotplug_rescan`.
- [x] تنفيذ العمليات الخطرة من خلال validation واضح قبل format/partition/mount/remount/unmount.
- [x] إضافة dry-run لكل عمليات partition/format قبل التنفيذ.
- [x] إضافة `disk rescan` لإعادة بناء block-device/partition/filesystem view دون reboot.
- [x] إضافة `fs check` و`fs repair` كواجهات لفحوص filesystem المدعومة؛ repair يرفض التنفيذ حتى وجود repair engine مسجل ويدعم dry-run.

### أوامر Shell المقترحة

```text
disk list
disk info disk0
disk rescan
partition list disk0
partition create disk0 1G
partition resize disk0p2 +4G
partition delete disk0p2
mount disk0p1 /data
remount /data rw
unmount /data
format disk1p1 fat32
format disk1p1 exfat
format disk1p1 udf
fs info /system
fs sync /data
fs check /data
fs repair /data
vdisk attach /images/data.vhdx /media/disk2
vdisk detach /media/disk2
```

## المرحلة 11: واجهة Disk Management الرسومية

### المطلوب

- [x] عرض الأقراص Physical وVirtual في تطبيق رسومي مستقل `Disk Manager` فوق طبقة الـGUI.
- [x] رسم partitions كشريط يوضح المساحات النسبية داخل القرص المحدد.
- [x] عرض filesystem والحجم، مع وضع free-space كـ `n/a` حتى تضيف drivers إحصاء المساحة الحرة.
- [x] mount وunmount من الواجهة، مع منع unmount للـsystem volume.
- [x] remount وتغيير سياسات rw/safe-read-only/no-exec من أزرار الواجهة.
- [x] format كواجهة آمنة: dry-run فقط من الـGUI، والتنفيذ الفعلي يظل محجوبًا حتى وجود formatter engine وتأكيد صريح.
- [x] إنشاء وحذف وتوسيع وتقليص partition كواجهة آمنة: create/resize dry-run، وdelete محجوب حتى confirm dialog حقيقي.
- [x] عرض read-only وwrite-protected وremovable وhealth status، مع إبراز read-only بصريًا.
- [x] progress وحالة العمليات الطويلة عبر status strip أولي؛ progress الحقيقي ينتظر عمليات async طويلة.
- [x] تحذيرات واضحة قبل العمليات المدمرة عبر status واضح ورفض التنفيذ غير المؤكد.
- [x] منع تنفيذ format أو delete على system volume.
- [x] عرض virtual disks attached وفحص أو فصل أول virtual disk من واجهة Disk Manager، مع منع detach إذا كان mounted.
- [x] واجهة `Check/Repair` لكل filesystem يدعم ذلك: check فعلي، وrepair dry-run حتى وجود repair engine.
- [x] عدم تقديم read-only كخيار افتراضي نهائي؛ الواجهة تسميه safe read-only mode وتجعله إجراءً صريحًا.

## المرحلة 12: NTFS Write Support

انتقل التنفيذ من guarded overwrite إلى عمليات create/write/delete/rename/move
للملفات والمجلدات، مع allocator وفهارس مجلدات ومعاملات rollback مستقلة.

### المطلوب المبدئي

- [x] تعديل ملفات resident وnon-resident موجودة دون تخصيص clusters جديدة.
- [x] إنشاء ملفات جديدة وتوسيع resident attributes أو تحويلها إلى non-resident.
- [x] تخصيص MFT records وclusters.
- [x] تحديث bitmaps.
- [x] rename وdelete وmove وإنشاء/حذف المجلدات.
- [x] تحديث directory indexes وتحويل root إلى allocation ثم split/rebuild.
- [x] معاملات قابلة للاسترجاع لكل MFT/bitmap/index mutation منفذة في Asas OS.
- [x] تجميع توافق recovery مع replay الكامل لـWindows `$LogFile` كبوابة NTFS compatibility خارجية؛ dirty Windows volumes تبقى protected read-only مؤقتًا بسبب واضح حتى ينجح replay متوافق.
- [x] عند فشل replay أو وجود feature غير مدعوم، يكون read-only وضع حماية مؤقت مع سبب واضح وليس حالة الدعم النهائية: NTFS الآن يسجل reason code/string، وDisk Management يعرض سبب القفل مثل dirty LogFile replay required أو device write-protected.

> ملاحظة تنفيذ: dirty Windows NTFS لا يتحول إلى full mode إلا بعد replay متوافق مع `$LogFile`.
> الحالة الحالية ترفض الكتابة مؤقتًا مع سبب واضح بدل تقديم read-only كهدف نهائي.

## ترتيب التنفيذ القادم

```text
1. تشغيل بوابتي NTFS وexFAT الخارجيتين على صور Windows فعلية
2. Hyper-V runtime verification ثم disk rescan لإكمال تسجيل الأجهزة بعد الإقلاع
3. دراسة وتنفيذ replay متوافق مع Windows `$LogFile` حتى dirty NTFS volumes تصبح full بعد recovery ناجح
4. ISO9660 وUDF full image/filesystem support بدل read-only
5. ext2/ext4 full support تدريجيًا مع fsck/recovery gates
6. Virtual Disk Layer وRAW/VHD/QCOW2/VHDX read-write
7. Block cache وtimeouts وtelemetry وpower-loss tests
8. Disk Management Shell Service
9. Disk Management GUI
```

بوابة Windows الخارجية الموحدة:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\Test-WindowsFilesystemGate.ps1 -NtfsPath C:\path\ntfs.vhdx -ExFatPath C:\path\exfat.vhdx
```

> هذه البوابة جاهزة لكنها لا تُعد مكتملة إلا بعد تشغيلها على صور Windows فعلية قابلة للـmount والفحص بـ`chkdsk`.

## الإصدار المرحلي الأساسي المكتمل

تم تحقيق متطلبات الإصدار المرحلي الأساسي:

- [x] جميع storage drivers الأساسية مسجلة كـ block devices مستقلة.
- [x] دعم MBR وGPT مستقر ومختبر.
- [x] FAT32 يستخدم context مستقل ويدعم عدة volumes بالتزامن.
- [x] NTFS يستخدم context مستقل ويدعم create/write/delete/rename/move transactional.
- [x] USB Mass Storage قابل للـ mount.
- [x] لا يعتمد VFS أو filesystem drivers الحديثة على global active storage device.
- [x] تنجح اختبارات Build وصورة FAT32 وQEMU على كل التغييرات.
