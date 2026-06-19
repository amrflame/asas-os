# خارطة طريق Asas OS

هذه الخريطة تلخص اتجاه التطوير العام. التفاصيل الكاملة موجودة في
`OS_DEVELOPMENT_PLAN_AR.md` و`DISK_MANAGEMENT_PLAN_AR.md`.

## التركيز الحالي

- إقلاع مستقر على Hyper-V Generation 2 وQEMU.
- Block Device Registry وMount Manager أكثر ثباتًا.
- واجهة Disk Management أكثر أمانًا للأقراص الفعلية والافتراضية.
- دعم FAT32 وNTFS وexFAT للقراءة والكتابة مع rollback وvalidation.
- تجهيز التوثيق والمساهمة المجتمعية قبل النشر العام.

## العمل القريب

- تحسين releases والصور التوضيحية.
- توسيع GitHub Actions.
- إضافة توثيق أوضح لإعداد بيئة Windows وQEMU وHyper-V.
- تحسين واجهة Disk Management.
- زيادة الاختبارات الآلية حول filesystem mutations.

## التخزين وأنظمة الملفات

- FAT32 يظل baseline مستقر للقراءة والكتابة.
- NTFS يستمر بدعم كتابة محمي مع بوابات توافق Windows.
- exFAT يحتاج تحقق أوسع على صور منشأة من Windows.
- ISO9660 وUDF وext2/ext4 وQCOW2 وVHDX full write تعتبر مراحل production
  gated تحتاج rollback وflush barriers وfsck/chkdsk خارجي قبل إعلانها كاملة.

## الأجهزة والمنصات

- Hyper-V Generation 2 هو هدف VM العام الرئيسي.
- QEMU هو هدف الاختبارات الآلية الرئيسي.
- اختبارات الأجهزة الفعلية مرحب بها، لكن يجب ألا تكسر مسارات VM.

## المجتمع

- استخدم label مثل `good first issue` للمهام الصغيرة.
- الأفضل Pull Requests صغيرة ومعها ملاحظات اختبار.
- العمليات الخطرة على الأقراص يجب أن تبقى خلف validation وdry-run وتأكيد صريح.

