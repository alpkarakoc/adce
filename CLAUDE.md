# ADCE — Anomaly Detection & Stochastic Containment Engine

Saf C11. Harici bağımlılık yok. Hedef: lock-free, tahsissiz, fail-closed yayın yolu.

## Komutlar

- `./scripts/verify.sh` — kapı: strict build + ASan/UBSan + TSan + test koşumu.
  Üçü de yeşil olmadan tur bitmez.
- Hızlı derleme: `cc -std=c11 -O2 -Wall -Wextra -Werror -Iinclude test/t_adce_platform.c -lpthread`

## Yapı

- `include/adce_platform.h` — tek başlık, C11 `stdatomic.h` acquire/release üzerine kurulu.
  Mutex, spinlock, tahsis yok.
- `test/t_adce_platform.c` — `t_*` birim testleri: Q16.16 aritmetiği (taşma dahil),
  token-bucket clamp, RNG sağlığı, zaman monotonluğu, tek-thread seqlock, iki-thread stress.

## Kilitli kararlar — gerekçesiz değiştirme

- `adce_epoch_state_t`: sequence + pressure (Q16.16) + epoch_id + observed_at_ns,
  tam 64 bayt, 64 bayt hizalı, `_Static_assert` ile sabit.
- `_Alignas` ilk üyeye uygulanır, typedef'e değil (C11 §6.7.5p2). Bu ilk derlemeyi kırdı.
- `adce_epoch_publish` / `adce_epoch_read` / `adce_epoch_is_stale`:
  publish/consume + fail-closed watchdog.
- Zaman: `CLOCK_MONOTONIC_RAW`.

## Çalışma anlaşması

- Placeholder yok. `// TODO`, stub dönüş, boş gövde defekttir.
- Uyarıyı bastırma, sebebini çöz. `-Werror` kaldırılmaz, suppression yazılmaz.
- Yazmadan önce oku. Var olmayan sembolü varsayma, grep et.
- Değişiklikler diff veya tam dosya olarak sunulur.
- Kilitli bir kararı ihlal eden istek geldiğinde dur, riski söyle, düzeltilmiş tasarımı öner.