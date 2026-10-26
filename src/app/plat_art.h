/* app/plat_art.h — Platform oyununun prosedürel pixel-art varlıkları (dış dosya yok).
 * 256×256 atlas (16 px karolar, 16 sütun): zemin/tuğla/taş/platform/diken/bayrak karoları,
 * altın kareleri, oyuncu ve düşman kareleri, süsler; ayrıca gökyüzü gradyanı ve dağ silueti.
 */
#ifndef APP_PLAT_ART_H
#define APP_PLAT_ART_H

#include "core/types.h"
#include "core/arena.h"
#include "raster/shade_abi.h"

/* Atlas indeksleri */
enum {
    ART_EMPTY        = 0,
    ART_GRASS        = 1,
    ART_DIRT         = 2,
    ART_BRICK        = 3,
    ART_STONE        = 4,
    ART_PLATFORM     = 5,
    ART_SPIKES       = 6,
    ART_FLAG_POLE    = 7,
    ART_FLAG_TOP     = 8,
    ART_WHITE        = 9,    /* düz beyaz (parçacıklar için) */
    ART_COIN0        = 16,   /* 16..19 dönen altın */
    ART_PLAYER_IDLE0 = 32,   /* 32,33 bekleme */
    ART_PLAYER_RUN0  = 34,   /* 34..37 koşu */
    ART_PLAYER_JUMP  = 38,
    ART_PLAYER_FALL  = 39,
    ART_PLAYER_HURT  = 40,
    ART_SLIME0       = 48,   /* 48,49 yürüme */
    ART_SLIME_FLAT   = 50,
    ART_BUSH         = 64,
    ART_FLOWER       = 65,
    ART_TUFT         = 66,
};

typedef struct {
    Texture atlas;      /* 256×256, en yakın komşu */
    Texture sky;        /* 4×64 dikey gradyan */
    Texture hills_far;  /* 256×64, yatayda döşenebilir */
    Texture hills_near; /* 256×64 */
} PlatArt;

void plat_art_build(PlatArt* art, Arena* perm, u64 seed);

#endif
