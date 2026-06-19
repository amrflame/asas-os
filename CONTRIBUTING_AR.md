# المساهمة في Asas OS

شكرًا لرغبتك في المساهمة. Asas OS نظام تشغيل تجريبي بروح تطوير مفتوحة:
افهم طبقة صغيرة، حسّنها، واتركها أوضح لمن يأتي بعدك. الهدف أن يتحول نظام صغير
إلى مشروع مجتمعي جاد يمكن التعلم منه والبناء عليه.

لا تحتاج أن تبدأ كخبير kernel. المساهمات الجيدة قد تكون توثيقًا، اختبارات،
صورًا، أوامر shell، تحسينات GUI، ملاحظات عتاد، تحقق من VM، أو بلاغ bug دقيق.
ولو لديك خبرة systems programming، فهناك عمل عميق في storage وfilesystems
وmemory وscheduling وnetworking وboot.

## حالة المشروع

Asas OS ليس نظام تشغيل إنتاجيًا. هو مشروع بحثي وتعليمي يركز على UEFI boot،
أساسيات النواة، تعريفات التخزين، أنظمة الملفات، VFS، الواجهة الرسومية،
ودعم بيئات المحاكاة والافتراضية. الهدف أن نجعل هندسة أنظمة التشغيل قابلة
للاقتراب بدون التظاهر أن الأجزاء الخطرة بسيطة.

كود الأقراص وأنظمة الملفات والجداول الافتراضية قد يسبب تلف بيانات إذا كان
خاطئًا. أي مساهمة في هذه المناطق يجب أن تكون محافظة ومعها ملاحظات اختبار واضحة.

## البداية

1. اختر مساحة صغيرة تهمك: docs أو tests أو GUI أو shell أو storage أو
   filesystems أو networking أو boot.
2. اعمل Fork للمستودع.
3. أنشئ branch مخصصًا من الفرع الافتراضي.
4. ابن المشروع محليًا على Windows:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\New-AsasIso.ps1 -Build
```

5. شغل الاختبار المناسب عند الإمكان:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\Run-QemuBootTest.ps1
```

6. افتح Pull Request بوصف واضح ونتائج تحقق.

## مساهمات مناسبة كبداية

- تحسين README أو الصور أو الرسوم التوضيحية أو ملاحظات الإعداد.
- إضافة أمثلة ناقصة لأوامر shell.
- إضافة ملاحظات اختبار QEMU/Hyper-V لإعداد جربته.
- تحسين رسائل الأخطاء في shell أو GUI.
- إضافة تشخيصات safe read-only للحالات غير المدعومة في الأقراص أو filesystems.
- كتابة أو توسيع اختبارات path parsing أو VFS أو image tools.

## مساحات أعمق للمساهمة

- اعتمادية block devices وtelemetry وcache وflush barriers.
- FAT32 وNTFS وexFAT وISO9660 وUDF وext2/ext4 validation وmutation paths.
- تجربة Disk Manager لعمليات mount وremount وformat وcheck وrepair بشكل آمن.
- Scheduler وprocess lifecycle وsyscalls وPE user-program loading.
- تجارب network stack وتشخيص packets أفضل.
- تقارير hardware compatibility على QEMU وHyper-V والأجهزة الفعلية.

## قواعد المساهمة

- اجعل التغيير محدودًا. الأفضل feature أو fix واحد لكل Pull Request.
- استخدم واجهات C بسيطة بين أجزاء النواة.
- لا تفترض وجود hosted C runtime داخل kernel code.
- لا تحذف مسارات fallback للأجهزة إلا بعد توفير بديل مختبر.
- اشرح المفاضلة في Pull Request عند تغيير boot أو storage أو memory أو كود
  حساس أمنيًا.
- لا تفتح الكتابة على filesystem أو disk format إلا بعد فهم rollback وbounds
  checks وflush behavior.
- وضع read-only حالة حماية مؤقتة، وليس هدفًا نهائيًا، إلا إذا كان العتاد نفسه
  read-only فعلًا.
- لا ترفع ملفات ناتجة مثل ISO وIMG وVHD وVHDX وQCOW2 وobject files والlogs.

## تغييرات التخزين وأنظمة الملفات

لو التغيير في storage أو partitioning أو filesystem أو virtual disks، اذكر:

- نوع image أو الجهاز المستخدم في الاختبار.
- هل الاختبار كان QEMU أو Hyper-V أو host-side فقط.
- ما الذي تم التحقق منه في read/write/mount/remount.
- أي اختبار corruption أو fsck أو chkdsk أو rollback أو power-loss.
- أي feature flags غير مدعومة أو safe read-only gates.

العمليات الخطرة يجب أن يكون لها validation وdry-run قبل إتاحة التنفيذ المدمر
للمستخدم.

## أسلوب الكود

- اتبع أسلوب الملف المحيط بالتغيير.
- استخدم الأنواع الصريحة الموجودة في المشروع.
- اجعل التعليقات قصيرة ومفيدة.
- لا تخلط refactor واسع مع تغيير سلوكي في نفس Pull Request.

## قائمة مراجعة Pull Request

- المشروع يبني بنجاح.
- لم يتم رفع artifacts مولدة.
- السلوك الظاهر للمستخدم موثق عند الحاجة.
- تم إضافة اختبارات أو ملاحظات تحقق.
- الأوامر أو أفعال GUI الجديدة تفشل بأمان عند عدم الدعم.
- يستطيع المراجع فهم الهدف بدون أن يستنتجه وحده من diff.

## الترخيص

بالمساهمة في المشروع، توافق على ترخيص مساهمتك تحت GNU General Public License
v3.0 only، مثل ترخيص هذا المستودع.
