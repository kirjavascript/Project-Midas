/**
 * funguns.c - the PC port's Portal Gun.
 *
 * It reuses ITEM_SILVERWPPK, a slot no level places, so weapon cycling, "all
 * guns" and the setup scripts keep working, re-skinned with the Moonraker
 * laser model. Fire (Z) shoots the blue portal, the aim button (R / right
 * click) the orange one (bondview2.c routes it here instead of entering aim
 * mode). Walking (or falling, or flying) into one puts Bond out of the other,
 * facing out of it, with his momentum turned through (bhop air velocity
 * horizontally, field_7C up/down).
 *
 * [input] portal_gun=1 (default) gives it at solo mission start; console
 * `give portalgun` adds and equips it any time. GE_FUNGUNDBG=1 logs
 * placement and teleports.
 */
#ifndef TARGET_N64
#include <ultra64.h>
#include <bondgame.h>
#include <bondtypes.h>
#include <bondconstants.h>
#include "bg.h"
#include "bondinv.h"
#include "bondview.h"
#include "chr.h"
#include "chrai.h"
#include "chraction.h"
#include "gun.h"
#include "loadobjectmodel.h"
#include "lv.h"
#include "math_atan2f.h"
#include "matrixmath.h"
#include "player.h"
#include "propobj.h"
#include "stan.h"

extern float sqrtf(float);
extern float sinf(float);
extern float cosf(float);
extern float fabsf(float);
extern char *getenv(const char *);
extern void portConLogf(const char *fmt, ...);

extern s32 portJoyPortPortalGun(void);
extern s32 portBhopGetVelocity(f32 *vx, f32 *vz);
extern void portBhopSetVelocity(f32 vx, f32 vz);
extern void portPortalDecalSet(s32 i, coord3d *pos, coord3d *normal, s32 room, f32 halfw, f32 halfh, u8 r, u8 g, u8 b);
extern void portPortalDecalClear(s32 i);

extern GunModelFileRecord gitem_structs[];
extern ModelFileHeader laser_header;
extern WeaponStats laser_stats;

extern void bullet_path_from_screen_center(coord3d *origin, coord3d *dir, enum GUNHAND hand);
extern s32 bgCopyDrawnRoomsToList(s32 *rooms, s32 max);
extern s32 bgTestBulletHitBackground(coord3d *from, coord3d *to, s32 room, HitThing *hit);
extern f32 get_room_data_float2(void);
extern void change_player_pos_to_target(struct collision434 *col, coord3d *pos, StandTile *stan);
extern void bondviewApplyVertaTheta(void);
extern void bondviewUpdatePlayerRoom(struct player *player);
extern f32 bondviewYPositionRelated(StandTile *tile, f32 x, f32 z);

#define PORTAL_GUN        ITEM_SILVERWPPK

#define PORTAL_HALF_W     35.0f
#define PORTAL_HALF_H     60.0f
#define PORTAL_COOLDOWN   20.0f    /* ticks after a trip before either portal works */
#define PORTAL_MIN_SPEED  3.0f     /* exit speed floor along the exit normal, units/tick */

static const char *const sFunGunStrings[] = {
    "Portal Gun\n",      /* 0 */
    "\n",                /* 1 */
};
#define FUNSTR(slot) ((u16) ((45 << 10) | (slot)))

typedef struct
{
    s32 active;
    coord3d pos;      /* world, on the surface */
    coord3d normal;   /* unit, out of the surface */
    s32 room;
} Portal;

static Portal s_portal[2];
static f32 s_portalCooldown;

static WeaponStats s_portalStats;
static s32 s_patched;

static s32 funDbg(void)
{
    static s32 dbg = -1;

    if (dbg < 0)
    {
        dbg = getenv("GE_FUNGUNDBG") != NULL;
    }
    return dbg;
}

const char *portFunGunsString(s32 slot)
{
    if ((slot < 0) || (slot >= (s32) (sizeof(sFunGunStrings) / sizeof(sFunGunStrings[0]))))
    {
        return "\n";
    }
    return sFunGunStrings[slot];
}

/* The item row, re-pointed once: the asset tables stay N64-exact. */
static void funPatchItems(void)
{
    GunModelFileRecord *portal = &gitem_structs[PORTAL_GUN];

    if (s_patched)
    {
        return;
    }
    s_patched = 1;

    s_portalStats = laser_stats;
    s_portalStats.Inaccuracy = 0.0f;
    s_portalStats.BitFlags &= ~WEAPONSTATBITFLAG_HAS_AUTO_AIM;
    s_portalStats.BitFlags |= WEAPONSTATBITFLAG_HIDE_AMMO_DISPLAY;

    portal->item_header = &laser_header;
    portal->item_file_name = "GlaserZ";
    portal->item_weapon_stats = &s_portalStats;
    portal->watch_pos_x = gitem_structs[ITEM_LASER].watch_pos_x;
    portal->watch_pos_y = gitem_structs[ITEM_LASER].watch_pos_y;
    portal->watch_pos_z = gitem_structs[ITEM_LASER].watch_pos_z;
    portal->equip_watch_x = gitem_structs[ITEM_LASER].equip_watch_x;
    portal->equip_watch_y = gitem_structs[ITEM_LASER].equip_watch_y;
    portal->equip_watch_z = gitem_structs[ITEM_LASER].equip_watch_z;
    portal->upper_watch_text = FUNSTR(1);
    portal->lower_watch_text = FUNSTR(0);
    portal->weapon_of_choice_text = FUNSTR(0);
    portal->watch_equipment_text = FUNSTR(0);
}

/** bondview_r.c, every level start after the fist is added. */
void portFunGunsLevelStart(void)
{
    s32 i;

    funPatchItems();
    for (i = 0; i < 2; i++)
    {
        s_portal[i].active = 0;
        portPortalDecalClear(i);
    }
    s_portalCooldown = 0.0f;
    if (portJoyPortPortalGun() && (getPlayerCount() == 1))
    {
        bondinvAddInvItem(PORTAL_GUN);
    }
}

/** Console `give`: adds and equips. 1 = done. */
s32 portFunGunsGive(const char *what)
{
    if ((what == NULL) || ((what[0] != 'p') && (what[0] != 'P')))
    {
        return 0;
    }
    funPatchItems();
    bondinvAddInvItem(PORTAL_GUN);
    currentPlayerEquipWeaponWrapper(GUNRIGHT, PORTAL_GUN);
    return 1;
}

static s32 funOn(void)
{
    return s_patched && (getPlayerCount() == 1);
}

/** gun.c: the Data/Textures/Models/ folder for a first-person gun, or NULL. */
const char *portFunGunModelDir(s32 item)
{
    return (s_patched && (item == PORTAL_GUN)) ? "portalgun" : NULL;
}

/** bondview2.c: 1 while the Portal Gun is in the right hand (aim = its second trigger). */
s32 portFunGunHeld(void)
{
    const s32 item = getCurrentPlayerWeaponId(GUNRIGHT);

    return funOn() && (item == PORTAL_GUN);
}

/* ---- aiming -------------------------------------------------------------- */

/* The crosshair ray in world space (the bullet's; the gun has 0 spread). */
static void funAimRay(coord3d *eye, coord3d *dir)
{
    coord3d o;
    coord3d d;

    bullet_path_from_screen_center(&o, &d, GUNRIGHT);
    *eye = o;
    mtx4TransformVecInPlace(currentPlayerGetViewToWorldMtxf(), eye);
    *dir = d;
    mtx4RotateVecInPlace(currentPlayerGetViewToWorldMtxf(), dir);
}

/**
 * Closest level-geometry hit along eye + dir * [0, maxdist] over the rooms
 * drawn this frame. bgTestBulletHitBackground reports the whole line in
 * scaled units, so hits are scaled back and kept to the forward segment.
 */
static s32 funTrace(coord3d *eye, coord3d *dir, f32 maxdist, coord3d *hitpos, coord3d *normal, s32 *hitroom)
{
    s32 rooms[MAXROOMCOUNT];
    s32 n = bgCopyDrawnRoomsToList(rooms, MAXROOMCOUNT);
    const f32 scale = get_room_data_float2();
    coord3d to;
    f32 best = maxdist;
    s32 found = 0;
    s32 i;

    to.x = eye->x + (dir->x * maxdist);
    to.y = eye->y + (dir->y * maxdist);
    to.z = eye->z + (dir->z * maxdist);

    for (i = 0; i < n; i++)
    {
        HitThing hit;
        coord3d p;
        f32 t;

        if ((rooms[i] <= 0) || !bgTestBulletHitBackground(eye, &to, rooms[i], &hit))
        {
            continue;
        }
        p.x = hit.hitpos.x * scale;
        p.y = hit.hitpos.y * scale;
        p.z = hit.hitpos.z * scale;
        t = ((p.x - eye->x) * dir->x) + ((p.y - eye->y) * dir->y) + ((p.z - eye->z) * dir->z);
        if ((t <= 0.0f) || (t >= best))
        {
            continue;
        }
        best = t;
        *hitpos = p;
        normal->x = hit.normal.x;
        normal->y = hit.normal.y;
        normal->z = hit.normal.z;
        *hitroom = rooms[i];
        found = 1;
    }
    if (found)
    {
        f32 len = sqrtf((normal->x * normal->x) + (normal->y * normal->y) + (normal->z * normal->z));

        if (len <= 0.0f)
        {
            return 0;
        }
        normal->x /= len;
        normal->y /= len;
        normal->z /= len;
        /* face the shooter */
        if (((normal->x * dir->x) + (normal->y * dir->y) + (normal->z * dir->z)) > 0.0f)
        {
            normal->x = -normal->x;
            normal->y = -normal->y;
            normal->z = -normal->z;
        }
    }
    return found;
}

/* ---- Portal Gun ---------------------------------------------------------- */

static void portalPlace(s32 which)
{
    coord3d eye, dir, pos, normal;
    s32 room;
    Portal *p = &s_portal[which];
    const Portal *other = &s_portal[!which];

    funAimRay(&eye, &dir);
    if (!funTrace(&eye, &dir, 20000.0f, &pos, &normal, &room))
    {
        return;
    }
    if (other->active)
    {
        const f32 dx = pos.x - other->pos.x;
        const f32 dy = pos.y - other->pos.y;
        const f32 dz = pos.z - other->pos.z;

        if (((dx * dx) + (dy * dy) + (dz * dz)) < (2.0f * PORTAL_HALF_W) * (2.0f * PORTAL_HALF_W))
        {
            return;   /* on top of the other one */
        }
    }
    p->active = 1;
    p->pos = pos;
    p->normal = normal;
    p->room = room;
    /* lifted off the surface a touch so it never z-fights the wall */
    {
        coord3d draw;

        draw.x = pos.x + (normal.x * 0.5f);
        draw.y = pos.y + (normal.y * 0.5f);
        draw.z = pos.z + (normal.z * 0.5f);
        if (which == 0)
        {
            portPortalDecalSet(0, &draw, &normal, room, PORTAL_HALF_W, PORTAL_HALF_H, 0x20, 0x80, 0xFF);
        }
        else
        {
            portPortalDecalSet(1, &draw, &normal, room, PORTAL_HALF_W, PORTAL_HALF_H, 0xFF, 0x80, 0x10);
        }
    }
    if (funDbg())
    {
        portConLogf("ge: portal %s at (%.0f %.0f %.0f) n=(%.2f %.2f %.2f) room %d\n", which ? "orange" : "blue",
                    pos.x, pos.y, pos.z, normal.x, normal.y, normal.z, room);
    }
}

static s32 portalIsWall(const Portal *p)
{
    return fabsf(p->normal.y) < 0.7f;
}

/* Bond is in portal `p`'s mouth this tick. */
static s32 portalTouching(const Portal *p, f32 radius)
{
    const coord3d *c = &g_CurrentPlayer->field_488.collision_position;
    const f32 feet = g_CurrentPlayer->field_70;
    const f32 dx = c->x - p->pos.x;
    const f32 dz = c->z - p->pos.z;

    if (portalIsWall(p))
    {
        const f32 h = sqrtf((p->normal.x * p->normal.x) + (p->normal.z * p->normal.z));
        const f32 nx = p->normal.x / h;
        const f32 nz = p->normal.z / h;
        const f32 depth = (dx * nx) + (dz * nz);
        const f32 side = (dx * -nz) + (dz * nx);

        /* his body (feet up to chest) overlaps the portal's height */
        return (depth < radius + 8.0f) && (depth > -radius) && (fabsf(side) < PORTAL_HALF_W - 5.0f)
            && (feet < p->pos.y + PORTAL_HALF_H) && ((feet + (g_CurrentPlayer->eyeheight * 0.7f)) > p->pos.y - PORTAL_HALF_H);
    }
    if (((dx * dx) + (dz * dz)) > (PORTAL_HALF_W * PORTAL_HALF_W))
    {
        return 0;
    }
    if (p->normal.y > 0.0f)
    {
        return (feet < p->pos.y + 4.0f) && (feet > p->pos.y - 60.0f);
    }
    return (feet + g_CurrentPlayer->eyeheight + 15.0f) > p->pos.y;
}

/* Bond out of `b`, having gone into `a`. */
static void portalTeleport(const Portal *a, const Portal *b)
{
    struct collision434 *col = &g_CurrentPlayer->field_488;
    const f32 radius = col->collision_radius;
    const f32 eyeh = g_CurrentPlayer->eyeheight;
    coord3d vin;
    coord3d vout;
    coord3d exitpos;
    coord3d probe;
    StandTile *tile;
    f32 floory;
    f32 feet;
    f32 speed;
    f32 into;
    f32 yawdelta = 0.0f;

    /* incoming velocity: the bhop air velocity, else what he just moved */
    if (!portBhopGetVelocity(&vin.x, &vin.z))
    {
        const f32 d = (g_GlobalTimerDelta > 0.0f) ? g_GlobalTimerDelta : 1.0f;

        vin.x = (col->collision_position.x - g_CurrentPlayer->bondprevpos.x) / d;
        vin.z = (col->collision_position.z - g_CurrentPlayer->bondprevpos.z) / d;
    }
    vin.y = g_CurrentPlayer->field_7C;
    speed = sqrtf((vin.x * vin.x) + (vin.y * vin.y) + (vin.z * vin.z));
    into = -((vin.x * a->normal.x) + (vin.y * a->normal.y) + (vin.z * a->normal.z));

    if (portalIsWall(a) && portalIsWall(b))
    {   /* turn everything (view and velocity) by the yaw between the portals */
        const f32 ain = atan2f(-a->normal.x, -a->normal.z);
        const f32 bout = atan2f(b->normal.x, b->normal.z);
        const f32 r = bout - ain;
        const f32 cr = cosf(r);
        const f32 sr = sinf(r);

        vout.x = (vin.x * cr) + (vin.z * sr);
        vout.z = (vin.z * cr) - (vin.x * sr);
        vout.y = vin.y;
        yawdelta = -r * (180.0f / M_PI_F);
    }
    else
    {   /* floor / ceiling: the speed goes straight out of the exit */
        const f32 out = (speed > PORTAL_MIN_SPEED) ? speed : PORTAL_MIN_SPEED;

        vout.x = b->normal.x * out;
        vout.y = b->normal.y * out;
        vout.z = b->normal.z * out;
        if (portalIsWall(b))
        {   /* face out of a wall exit */
            const f32 theta = atan2f(-b->normal.x, b->normal.z);

            yawdelta = (theta * (180.0f / M_PI_F)) - g_CurrentPlayer->vv_theta;
        }
    }
    (void) into;

    if (portalIsWall(b))
    {
        const f32 h = sqrtf((b->normal.x * b->normal.x) + (b->normal.z * b->normal.z));

        exitpos.x = b->pos.x + ((b->normal.x / h) * (radius + 12.0f));
        exitpos.z = b->pos.z + ((b->normal.z / h) * (radius + 12.0f));
        feet = b->pos.y - PORTAL_HALF_H + 10.0f;
    }
    else if (b->normal.y > 0.0f)
    {
        exitpos.x = b->pos.x;
        exitpos.z = b->pos.z;
        feet = b->pos.y + 2.0f;
    }
    else
    {
        exitpos.x = b->pos.x;
        exitpos.z = b->pos.z;
        feet = b->pos.y - eyeh - 25.0f;
    }

    probe.x = exitpos.x;
    probe.y = (portalIsWall(b) ? b->pos.y : feet) + 10.0f;
    probe.z = exitpos.z;
    tile = stanFindTileBelowPos(&probe, NULL, &floory);
    if (tile == NULL)
    {
        if (funDbg())
        {
            portConLogf("ge: portal exit has no floor under it\n");
        }
        return;
    }
    if (feet < floory)
    {
        feet = floory;
    }

    exitpos.y = feet + eyeh;
    change_player_pos_to_target(col, &exitpos, tile);
    col->collision_radius = radius;   /* that reset it to 30 */
    g_CurrentPlayer->field_70 = feet;
    g_CurrentPlayer->stanHeight = bondviewYPositionRelated(tile, exitpos.x, exitpos.z);
    g_CurrentPlayer->field_6C = feet / 0.17000002f;
    g_CurrentPlayer->field_7C = vout.y;
    g_CurrentPlayer->field_2A6C = 0;
    g_CurrentPlayer->field_2A70 = NULL;

    g_CurrentPlayer->vv_theta += yawdelta;
    while (g_CurrentPlayer->vv_theta < 0.0f)
    {
        g_CurrentPlayer->vv_theta += 360.0f;
    }
    while (g_CurrentPlayer->vv_theta >= 360.0f)
    {
        g_CurrentPlayer->vv_theta -= 360.0f;
    }
    bondviewApplyVertaTheta();

    g_CurrentPlayer->prop->pos = exitpos;
    g_CurrentPlayer->bondprevpos = exitpos;
    g_CurrentPlayer->prop->stan = tile;
    g_CurrentPlayer->field_3B8.x = col->pos.x / 0.100000024f;
    g_CurrentPlayer->field_3B8.y = col->pos.y / 0.100000024f;
    g_CurrentPlayer->field_3B8.z = col->pos.z / 0.100000024f;
    bondviewUpdatePlayerRoom(g_CurrentPlayer);

    portBhopSetVelocity(vout.x, vout.z);
    s_portalCooldown = PORTAL_COOLDOWN;

    if (funDbg())
    {
        portConLogf("ge: portal trip -> (%.0f %.0f %.0f) v=(%.2f %.2f %.2f) yaw%+.0f\n",
                    exitpos.x, feet, exitpos.z, vout.x, vout.y, vout.z, yawdelta);
    }
}

static void portalTick(void)
{
    const f32 radius = g_CurrentPlayer->field_488.collision_radius;
    s32 i;

    if (s_portalCooldown > 0.0f)
    {
        s_portalCooldown -= g_GlobalTimerDelta;
        return;
    }
    if (!s_portal[0].active || !s_portal[1].active)
    {
        return;
    }
    for (i = 0; i < 2; i++)
    {
        if (portalTouching(&s_portal[i], radius))
        {
            portalTeleport(&s_portal[i], &s_portal[!i]);
            return;
        }
    }
}

/* ---- triggers ------------------------------------------------------------ */

/** chrprop.c chraiCheckUseHeldItem: the fire trigger. 1 = handled. */
s32 portFunGunFire(s32 hand, s32 item)
{
    if (!funOn() || (hand != GUNRIGHT) || (item != PORTAL_GUN))
    {
        return 0;
    }
    portalPlace(0);
    return 1;
}

/** bondview2.c: the aim button while the Portal Gun is out. */
void portFunGunAlt(void)
{
    if (getCurrentPlayerWeaponId(GUNRIGHT) == PORTAL_GUN)
    {
        portalPlace(1);
    }
}

/** bondview2.c, every on-foot tick after Bond's move. */
void portFunGunsTick(void)
{
    if (funOn())
    {
        portalTick();
    }
}
#endif
