---
paths:
  - "include/**/*.h"
  - "src/**/*.c"
  - "test/**/*.c"
---

# ADCE eşzamanlılık kuralları

- C11 bellek modelinde veri yarışı tanımsız davranıştır. Seqlock bunun istisnası DEĞİLDİR.
  Eşzamanlı erişilen her payload alanı `_Atomic` olmalı ve `memory_order_relaxed` ile
  okunup yazılmalıdır.
- Seqlock yazar: seq artır (release) -> payload (relaxed) -> seq artır (release).
  Seqlock okuyucu: seq oku (acquire) -> payload (relaxed) ->
  `atomic_thread_fence(memory_order_acquire)` -> seq'i yeniden oku ve karşılaştır.
- Doğru yazılmış bir seqlock TSan altında TEMİZDİR. TSan raporu çıkıyorsa protokol
  eksiktir. Suppression dosyası yazmak yasaktır; sorunu bellek düzeninde çöz.
- Mutex, spinlock, malloc/free yayın yolunda (publish path) yasaktır.
- Zaman kaynağı yalnızca `CLOCK_MONOTONIC_RAW`. `CLOCK_REALTIME` geriye atlar.
- Yayın nesnesi tam olarak bir 64 baytlık cache line; `_Alignas` ilk üyeye uygulanır
  (C11 §6.7.5p2 typedef üzerinde `_Alignas`'ı yasaklar), boyut `_Static_assert` ile sabitlenir.
- Q16.16 aritmetiğinde ara çarpımlar `int64_t`'ye genişletilir, taşma sınırda test edilir.
- Yeni davranış, testiyle aynı commit'te gelir. `./scripts/verify.sh` üç profilde de yeşil
  olmadan iş bitmiş sayılmaz.