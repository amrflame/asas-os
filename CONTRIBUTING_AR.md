# المساهمة في Asas OS

شكرًا لرغبتك في المساهمة. Asas OS نظام تشغيل تجريبي، لذلك التغييرات الصغيرة
المحددة والمختبرة أفضل من إعادة كتابة كبيرة يصعب مراجعتها.

## حالة المشروع

Asas OS ليس نظام تشغيل إنتاجيًا. هو مشروع بحثي وتعليمي يركز على UEFI boot،
أساسيات النواة، تعريفات التخزين، أنظمة الملفات، VFS، الواجهة الرسومية،
ودعم بيئات المحاكاة والافتراضية.

كود الأقراص وأنظمة الملفات والجداول الافتراضية قد يسبب تلف بيانات إذا كان
خاطئًا. أي مساهمة في هذه المناطق يجب أن تكون محافظة ومعها ملاحظات اختبار واضحة.

## البداية

1. اعمل Fork للمستودع.
2. أنشئ branch مخصصًا من الفرع الافتراضي.
3. ابن المشروع محليًا على Windows:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\New-AsasIso.ps1 -Build
```

4. شغل الاختبار المناسب عند الإمكان:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\Run-QemuBootTest.ps1
```

5. افتح Pull Request بوصف واضح ونتائج تحقق.

## قواعد المساهمة

- اجعل التغيير محدودًا. الأفضل feature أو fix واحد لكل Pull Request.
- استخدم واجهات C بسيطة بين أجزاء النواة.
- لا تفترض وجود hosted C runtime داخل kernel code.
- لا تحذف مسارات fallback للأجهزة إلا بعد توفير بديل مختبر.
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

## الترخيص

بالمساهمة في المشروع، توافق على ترخيص مساهمتك تحت GNU General Public License
v3.0 only، مثل ترخيص هذا المستودع.

