/* phys2/character.h — kinematik karakter kontrolcüsü (kapsül = 3 küre).
 * Fizik dünyasında gövde DEĞİLDİR; dünyayı sorgular, dinamik gövdeleri iter.
 * Özellikler: kaydırarak çarpışma çözme, eğim sınırı (yürünebilir yüzeyde yalnızca dikey itme →
 * eğimden kaymaz), basamak çıkma (yukarı → ileri → aşağı), zemine yapışma, zıplama, hava kontrolü.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#ifndef PHYS2_CHARACTER_H
#define PHYS2_CHARACTER_H
#include "phys2/phys.h"

typedef struct Phys2CharacterDesc {
    Vec3 pos;           /* ayak noktası */
    f32  radius, height;
    f32  step_height;
    f32  max_slope_deg;
    f32  speed, jump_speed;
    f32  mass;          /* dinamik gövdeleri iterken */
} Phys2CharacterDesc;

typedef struct Phys2Character {
    Vec3 pos;
    Vec3 vel;
    f32  radius, height, step_height, slope_cos, speed, jump_speed, mass;
    b32  grounded;
    Vec3 ground_normal;
    u32  ground_body;
    u32  last_contacts;   /* son güncellemede çözülen girişim sayısı */
    b32  stepped;         /* son güncellemede basamak çıktı mı */
    u32  last_tests;      /* son güncellemede dar fazda sınanan gövde sayısı (geniş faz: ~ yakındaki gövdeler, O(N) değil) */
} Phys2Character;

Phys2CharacterDesc phys2_character_desc_default(Vec3 pos);
void phys2_character_init(Phys2Character* c, const Phys2CharacterDesc* d);
/* wish: yatay istenen yön (|wish| <= 1), dünya uzayında */
void phys2_character_update(Phys2World* w, Phys2Character* c, Vec3 wish, b32 jump, f32 dt);
Vec3 phys2_character_eye(const Phys2Character* c);

#endif
