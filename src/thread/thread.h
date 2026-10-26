/* thread/thread.h — Thread kimliği, yığın (stack), TLS ve JIT eşitleme altyapısı.
 * Her worker thread için 1 MB yığın ve TLS alanı ayırır; __stack_pointer ve __tls_base
 * global yazmaçlarını kurar. Ayrıca ana thread ile worker'lar arasındaki JIT tablo
 * çoğaltma el sıkışmasını (ack sayaçlarını) paylaşımlı bellekte yönetir.
 */
#ifndef THREAD_THREAD_H
#define THREAD_THREAD_H

#include "core/types.h"
#include "plat/platform.h"

/* Maksimum toplam thread sayısı: 1 ana thread (id=0) + 8 worker thread (id=1..8) */
#define THREAD_MAX_WORKERS 8u
#define THREAD_MAX_COUNT   (THREAD_MAX_WORKERS + 1u)
#define THREAD_STACK_SIZE  MB(1)
#define THREAD_TLS_SIZE    256u

/* JIT çoğaltma ack tablosu kapasitesi (tablo indeksleri 0..JIT_MAX_FUNCS-1) */
#define JIT_MAX_FUNCS      1024u

/* Geçerli thread'in kimliğini döndürür (0 = ana thread, 1..N = worker thread). */
u32  thread_id(void);

/* Toplam aktif thread sayısını döndürür (1 + aktif worker sayısı, en az 1). */
u32  thread_count(void);

/* Aktif worker sayısını döndürür (thread_count() - 1). */
u32  thread_worker_count(void);

/* Havuzda başlatılmış (hazır) toplam worker sayısını döndürür. */
u32  thread_spawned_worker_count(void);

/* Worker thread için ayrılmış 1 MB yığının üst adresini (16-bayt hizalı) döndürür. */
EXPORT(engine_thread_stack_top) u32 engine_thread_stack_top(u32 tid);

/* Worker thread başlatma giriş noktası: __stack_pointer ve __tls_base kurar,
 * thread_id değerini TLS'e yazar ve bu worker'ı hazır olarak işaretler.
 * stack_top == 0 verilirse dahili 1 MB yığın otomatik kullanılır.
 */
EXPORT(engine_thread_init) void engine_thread_init(u32 tid, u32 stack_top);

/* JS (threads.js) tarafından başlatılan hedef worker sayısını bildirir. */
EXPORT(engine_set_worker_count) void engine_set_worker_count(u32 num_workers);

/* Havuz kapatıldığında hazır worker sayacını sıfırlar. */
EXPORT(engine_reset_ready_workers) void engine_reset_ready_workers(void);

/* JIT çoğaltma için paylaşımlı ack dizisinin taban adresini döndürür (u32[JIT_MAX_FUNCS]). */
EXPORT(engine_jit_ack_buf) u32* engine_jit_ack_buf(void);

/* JIT fonksiyon indeksi idx için tüm aktif worker'ların tablo güncellemesini (ack)
 * tamamlamasını bekler (ana thread spin + zaman aşımı). Başarılıysa TRUE döner.
 */
b32  thread_wait_jit_ack(u32 idx);

#endif /* THREAD_THREAD_H */
