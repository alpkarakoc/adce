#!/usr/bin/env bash
# Her .c/.h yazımından sonra hızlı katı derleme. Ucuz; ihlali modelin önüne
# oturum sonunda değil, bir sonraki turda koyar.
set -uo pipefail

input="$(cat)"
path="$(printf '%s' "$input" | jq -r '.tool_input.file_path // empty')"
case "$path" in
  *.c|*.h) ;;
  *) exit 0 ;;
esac

cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0

# --- kaynak keşfi ------------------------------------------------------------
# Tek bir dosya yolunu sabitlemek, bakmadığı işi onaylayan bir kontrol üretir:
# src/adce_observe.c düzenlendiğinde bu hook temiz dönerdi, ama o dosyayı hiç
# derlememiş olurdu. Liste bu yüzden türetiliyor. Kural scripts/verify.sh ile
# aynı, ama elle kopyalandı: kapılar ayrı commit'lerde değişir, ortak bir dosya
# onları tek kapıya bağlardı.
#
# Glob'lar cd'den SONRA açılıyor; öncesinde açılsalardı hook başka bir dizinden
# çağrıldığında hiçbir şeyi denetlemezdi.
shopt -s nullglob
TEST_SRCS=(test/t_*.c)
LIB_SRCS=()
if [ -d src ]; then LIB_SRCS=(src/*.c); fi
shopt -u nullglob

if [ ${#TEST_SRCS[@]} -eq 0 ]; then
  printf 'test/t_*.c hiçbir dosyayla eşleşmedi; denetlenecek bir şey yok.\n' >&2
  exit 2
fi
# src/ henüz yok ve bu meşru. Var olup hiç .c içermemesi meşru değil: bir dosya
# silinmiş, yanlış adlandırılmış ya da hiç eklenmemiş demektir.
if [ -d src ] && [ ${#LIB_SRCS[@]} -eq 0 ]; then
  printf 'src/ var ama src/*.c hiçbir dosyayla eşleşmedi.\n' >&2
  exit 2
fi

# bash 3.2 (macOS sistem bash'i) 'set -u' altında "${boş[@]}" ifadesini hata sayar.
SRCS=(${LIB_SRCS[@]+"${LIB_SRCS[@]}"} "${TEST_SRCS[@]}")

# Tek cc çağrısı: bu hook her düzenlemeden sonra çalışıyor, dosya başına bir
# süreç pahalı olurdu. -fsyntax-only bağlama yapmadığı için birden çok test
# dosyası kendi main()'ini tanımlasa bile burada çakışma olmaz; bağlama
# çakışmasını yakalamak kapıların işi, bu kontrolün değil.
if ! out="$(cc -std=c11 -O2 -Wall -Wextra -Werror -Wconversion -pedantic \
             -fsyntax-only -Iinclude "${SRCS[@]}" 2>&1)"; then
  printf 'Strict C11 build broken by %s:\n%s\n' "$path" "$out" >&2
  exit 2
fi
exit 0
