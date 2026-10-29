/* phys2/soft.h — XPBD yumuşak cisimler: kumaş (mesafe kısıtları) ve jöle (tetrahedron hacim + kenar).
 * "Küçük adımlar" (Macklin 2019): adım başına N alt adım, her alt adımda tek Gauss-Seidel turu, λ sıfırdan.
 * Uyum (compliance) α = 1/k [m/N]; α̃ = α/h². Parçacıklar rijit dünyayla (phys/phys.h) iki yönlü çarpışır:
 * gövde pozu adım başı → sonu arası alt adımlara bölünür, parçacık dışarı itilir, gövdeye eşit ve ters itki.
 * Sıra sabit (dizi sırası) → belirleyici.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#ifndef PHYS2_SOFT_H
#define PHYS2_SOFT_H
#include "core/arena.h"
#include "phys2/phys.h"

#define PHYS2_SOFT_MAX_COLORS 32   /* son renk (31) taşma kovası: bağımsızlık garantisi yok, her zaman seri */

enum { PHYS2_SOFT_STRETCH, PHYS2_SOFT_SHEAR, PHYS2_SOFT_BEND, PHYS2_SOFT_EDGE, PHYS2_SOFT_TETHER /* tek yönlü: yalnızca l > rest iken */ };

typedef struct Phys2SoftDist {
    u32 a, b;
    f32 rest;
    f32 compliance;
    u32 kind;   /* SOFT_* */
} Phys2SoftDist;

typedef struct Phys2SoftTet {
    u32 v[4];
    f32 rest_vol;
} Phys2SoftTet;

typedef struct Phys2SoftBody {
    u32       count;
    Vec3*     x;          /* konum */
    Vec3*     prev;       /* alt adım başı konum */
    Vec3*     v;          /* hız */
    f32*      w;          /* ters kütle; 0 = iğneli (sabit) */
    Phys2SoftDist* dist;
    u32       dist_count;
    Phys2SoftTet*  tets;
    u32       tet_count;
    f32       volume_compliance;
    u32*      tris;       /* yüzey üçgenleri (çizim, rüzgâr); dışarıdan CCW */
    u32       tri_count;
    f32       radius;     /* parçacık çarpışma yarıçapı */
    f32       friction;
    f32       damping;    /* hız sönümü (1/s) */
    f32       drag;       /* rüzgâr: normal yönlü sürükleme katsayısı (½ρC_d); 0 = rüzgârsız */
    f32       mass;       /* toplam (iğnesiz parçacıklar) */
    Vec3      aabb_min, aabb_max;
    u32       contacts;   /* son adımda rijit temas eden parçacık-alt adım sayısı */
    /* Faz 8: kumaş */
    Vec3*     x0;         /* dinlenme konumları (öz-çarpışmada dinlenmede yakın çiftler hariç) */
    b32       self_collide;   /* parçacık-parçacık öz-çarpışma (uzamsal karma) */
    b32       tri_collide;    /* rijit gövde ↔ yüzey üçgeni teması (yavaş gövdeler; GJK/EPA dar faz) */
    b32       catch_fast;     /* hızlı gövdeyi (> 5 m/s) ağ gibi yakala: iz silindiri + iki yönlü tutma */
    u32*      hash_start; /* karma tablosu: hücre başlangıçları (hash_cap + 1) */
    u32*      hash_items;
    u32       hash_cap;
    /* renkli kısıt çözümü (phys2_soft_set_colored): aynı renkteki kısıtlar ortak parçacık paylaşmaz →
     * renk içi sıra sonucu değiştirmez; büyük gövdede renk grubu thread'lere bölünür (seri ile bit-bit aynı) */
    b32       colored;
    u32*      dist_order;                                 /* mesafe kısıt indeksleri, renk sırasıyla */
    u32       dist_color_start[PHYS2_SOFT_MAX_COLORS + 1];
    u32*      tet_order;
    u32       tet_color_start[PHYS2_SOFT_MAX_COLORS + 1];
} Phys2SoftBody;

/* hızlı rijit gövdenin yakaladığı kumaş parçacığı (ağ gibi): gövdeye göre sabit, kumaş gerginliği itkiyle
 * gövdeye döner; gövde yavaşlayınca / geri dönünce bırakılır */
typedef struct Phys2SoftAttach {
    u32  soft, particle, body;
    Vec3 offset;   /* parçacık − gövde merkezi (dünya, yakalama anında) */
    Vec3 dir;      /* yakalama anındaki hareket yönü */
} Phys2SoftAttach;

#define PHYS2_SOFT_MAX_ATTACH 256

typedef struct Phys2SoftWorld {
    Phys2SoftBody** bodies;
    u32        count, cap;
    Phys2SoftAttach* attach;
    u32        attach_count;
    u32        substeps;
    Vec3       gravity;
    Vec3       wind;      /* m/s; kumaşa etkir (drag > 0) */
    b32        two_way;   /* dinamik rijit gövdelere itki uygula */
    f64        step_ms;
    u32        rigid_contacts, soft_contacts;
    u32        tri_contacts;   /* rijit ↔ kumaş üçgeni (alt adım toplamı) */
    u32        self_contacts;  /* kumaş öz-çarpışma çifti (alt adım toplamı) */
    u32        threads;        /* 1 = hep seri; 0 = iş sistemindeki tüm thread'ler (renkli kısıtlar, parçacık döngüleri) */
    /* adım içi geçici durum (arena'dan, soft.c özel tipleri): aday gövde kopyaları, rijit yollar, sorgu tamponu */
    void*      cand_store;
    void*      paths;
    u32        path_count;
    u32*       query;
} Phys2SoftWorld;

void phys2_soft_world_init(Phys2SoftWorld* sw, Arena* a, u32 cap);
u32  phys2_soft_world_add(Phys2SoftWorld* sw, Phys2SoftBody* b);
/* rijit dünya bu dt ile az önce adımlandıysa (pos0/rot0 adım başı) gövde pozları alt adımlara bölünür */
void phys2_soft_world_step(Phys2SoftWorld* sw, Phys2World* rigid, f32 dt);

/* Kumaş: origin köşesinden du (nu parçacık) ve dv (nv parçacık) kenarları boyunca ızgara.
 * Kısıtlar: yapısal (komşu), kesme (köşegen), bükülme (iki ötedeki). Öz-çarpışma ve rijit-üçgen teması açık;
 * öz-çarpışma için parçacık alt adımda yarıçaptan fazla ilerleyemez (hız sınırı r/h). */
Phys2SoftBody* phys2_soft_cloth(Arena* a, Vec3 origin, Vec3 du, Vec3 dv, u32 nu, u32 nv, f32 mass,
                     f32 stretch_compliance, f32 shear_compliance, f32 bend_compliance);
/* Jöle küp: dış boyut size (parçacık kafesi size·cells/(cells+1), yarıçap kafes adımının yarısı → parçacık
 * zarfı tam size), kenar başına cells hücre (her hücre 5 tetrahedron). Çizimde yüzey normal boyunca
 * radius kadar dışarı itilmeli. */
Phys2SoftBody* phys2_soft_jelly(Arena* a, Vec3 center, f32 size, u32 cells, f32 mass, f32 edge_compliance, f32 volume_compliance);
/* parçacığı sabitle (ters kütle 0) */
void phys2_soft_pin(Phys2SoftBody* b, u32 i);
/* Uzun menzilli bağlar (Müller ve ark. 2012): iğnelerden sonra çağrılır; her serbest parçacık en yakın
 * iğneye dinlenme uzaklığından fazla uzaklaşamaz. Az iterasyonda zincir boyu uzamayı keser. Kapasite: arena. */
void phys2_soft_add_tethers(Arena* a, Phys2SoftBody* b);
void phys2_soft_set_velocity(Phys2SoftBody* b, Vec3 v);
/* Kısıtları graf renklendirmesiyle sırala (açgözlü, en çok 31 renk + taşma) ve çözümü renk sırasıyla yap:
 * sonuç dizi sırasındakinden farklı ama thread sayısından bağımsızdır; büyük gövdede renk grupları paralel.
 * Pin / tether ekledikten SONRA çağır (add_tethers kapatır). on = FALSE: dizi sırası (varsayılan). */
void phys2_soft_set_colored(Arena* a, Phys2SoftBody* b, b32 on);

f32  phys2_soft_volume(const Phys2SoftBody* b);          /* tetrahedronların işaretli hacim toplamı */
f32  phys2_soft_rest_volume(const Phys2SoftBody* b);
f32  phys2_soft_max_stretch(const Phys2SoftBody* b);     /* en büyük |l − l0| / l0 (yapısal / kenar kısıtları) */
Vec3 phys2_soft_momentum(const Phys2SoftBody* b);
u32  phys2_soft_hash(const Phys2SoftWorld* sw);

#endif
