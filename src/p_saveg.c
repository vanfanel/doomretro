/*
========================================================================

                           D O O M  R e t r o
         The classic, refined DOOM source port. For Windows PC.

========================================================================

  Copyright © 1993-2012 by id Software LLC, a ZeniMax Media company.
  Copyright © 2013-2020 by Brad Harding.

  DOOM Retro is a fork of Chocolate DOOM. For a list of credits, see
  <https://github.com/bradharding/doomretro/wiki/CREDITS>.

  This file is a part of DOOM Retro.

  DOOM Retro is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation, either version 3 of the License, or (at your
  option) any later version.

  DOOM Retro is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with DOOM Retro. If not, see <https://www.gnu.org/licenses/>.

  DOOM is a registered trademark of id Software LLC, a ZeniMax Media
  company, in the US and/or other countries, and is used without
  permission. All other trademarks are the property of their respective
  holders. DOOM Retro is in no way affiliated with nor endorsed by
  id Software.

========================================================================
*/

#include "am_map.h"
#include "c_console.h"
#include "doomstat.h"
#include "g_game.h"
#include "i_system.h"
#include "m_config.h"
#include "m_misc.h"
#include "p_local.h"
#include "p_saveg.h"
#include "p_setup.h"
#include "p_tick.h"
#include "s_sound.h"
#include "st_stuff.h"
#include "version.h"
#include "z_zone.h"

#define SAVEGAME_EOF    0x1D
#define TARGETLIMIT     4192

FILE        *save_stream;

static int  thingindex;
static int  targets[TARGETLIMIT];
static int  tracers[TARGETLIMIT];
static int  lastenemies[TARGETLIMIT];
static int  soundtargets[TARGETLIMIT];
static int  attacker;

// Get the filename of a temporary file to write the savegame to. After
// the file has been successfully saved, it will be renamed to the
// real file.
char *P_TempSaveGameFile(void)
{
    static char *filename;

    if (!filename)
        filename = M_StringJoin(savegamefolder, "temp.save", NULL);

    return filename;
}

// Get the filename of the save game file to use for the specified slot.
char *P_SaveGameFile(int slot)
{
    static char *filename;
    static int  filename_size;
    char        basename[32];

    if (!filename)
    {
        filename_size = (int)strlen(savegamefolder) + 32;
        filename = malloc(filename_size);
    }

    M_snprintf(basename, sizeof(basename), PACKAGE_SAVE, slot);
    M_snprintf(filename, filename_size, "%s%s", savegamefolder, basename);

    return filename;
}

// Endian-safe integer read/write functions
static byte saveg_read8(void)
{
    byte    result = -1;

    fread(&result, 1, 1, save_stream);

    return result;
}

static void saveg_write8(byte value)
{
    fwrite(&value, 1, 1, save_stream);
}

static short saveg_read16(void)
{
    int result;

    result = saveg_read8();
    result |= saveg_read8() << 8;

    return result;
}

static void saveg_write16(short value)
{
    saveg_write8(value & 0xFF);
    saveg_write8((value >> 8) & 0xFF);
}

static int saveg_read32(void)
{
    int result;

    result = saveg_read8();
    result |= saveg_read8() << 8;
    result |= saveg_read8() << 16;
    result |= saveg_read8() << 24;

    return result;
}

static void saveg_write32(int value)
{
    saveg_write8(value & 0xFF);
    saveg_write8((value >> 8) & 0xFF);
    saveg_write8((value >> 16) & 0xFF);
    saveg_write8((value >> 24) & 0xFF);
}

// Enum values are 32-bit integers.
#define saveg_read_enum     saveg_read32
#define saveg_write_enum    saveg_write32

#define saveg_read_bool     saveg_read8
#define saveg_write_bool    saveg_write8

//
// Structure read/write functions
//

//
// mapthing_t
//
static void saveg_read_mapthing_t(mapthing_t *str)
{
    str->x = saveg_read16();
    str->y = saveg_read16();
    str->angle = saveg_read16();
    str->type = saveg_read16();
    str->options = saveg_read16();
}

static void saveg_write_mapthing_t(mapthing_t *str)
{
    saveg_write16(str->x);
    saveg_write16(str->y);
    saveg_write16(str->angle);
    saveg_write16(str->type);
    saveg_write16(str->options);
}

static int P_ThingToIndex(mobj_t *thing)
{
    int i = 0;

    if (!thing)
        return 0;

    for (thinker_t *th = thinkers[th_mobj].cnext; th != &thinkers[th_mobj]; th = th->cnext)
    {
        i++;

        if ((mobj_t *)th == thing)
            return i;
    }

    return 0;
}

static mobj_t *P_IndexToThing(int index)
{
    int i = 0;

    if (!index)
        return NULL;

    for (thinker_t *th = thinkers[th_mobj].cnext; th != &thinkers[th_mobj]; th = th->cnext)
        if (++i == index)
            return (mobj_t *)th;

    return NULL;
}

//
// mobj_t
//
static void saveg_read_mobj_t(mobj_t *str)
{
    int state;

    str->x = saveg_read32();
    str->y = saveg_read32();
    str->z = saveg_read32();
    str->angle = saveg_read32();
    str->sprite = (spritenum_t)saveg_read_enum();
    str->frame = saveg_read32();
    str->floorz = saveg_read32();
    str->ceilingz = saveg_read32();
    str->dropoffz = saveg_read32();
    str->radius = saveg_read32();
    str->height = saveg_read32();
    str->momx = saveg_read32();
    str->momy = saveg_read32();
    str->momz = saveg_read32();
    str->type = (mobjtype_t)saveg_read_enum();
    str->tics = saveg_read32();
    str->state = ((state = saveg_read32()) > 0 && state < NUMSTATES ? &states[state] : NULL);
    str->flags = saveg_read32();
    str->flags2 = saveg_read32();
    str->health = saveg_read32();
    str->movedir = saveg_read32();
    str->movecount = saveg_read32();
    targets[thingindex] = saveg_read32();
    str->reactiontime = saveg_read32();
    str->threshold = saveg_read32();

    if (saveg_read_bool())
    {
        str->player = viewplayer;
        str->player->mo = str;
    }

    saveg_read_mapthing_t(&str->spawnpoint);
    tracers[thingindex] = saveg_read32();
    lastenemies[thingindex] = saveg_read32();
    str->floatbob = saveg_read32();
    str->shadowoffset = saveg_read32();
    str->gear = saveg_read16();
    str->bloodsplats = saveg_read32();
    str->blood = saveg_read32();
    str->interpolate = saveg_read32();
    str->oldx = saveg_read32();
    str->oldy = saveg_read32();
    str->oldz = saveg_read32();
    str->oldangle = saveg_read32();
    str->nudge = saveg_read32();
    str->pitch = saveg_read32();
    str->id = saveg_read32();
    str->pursuecount = saveg_read16();
    str->strafecount = saveg_read16();

    if (str->flags & MF_SHOOTABLE)
        for (int i = 0; i < 100; i++)
            str->name[i] = saveg_read8();

    str->madesound = saveg_read32();
    str->flags3 = saveg_read32();
    str->inflicter = saveg_read32();

    // [BH] For future features without breaking savegame compatibility
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
}

static void saveg_write_mobj_t(mobj_t *str)
{
    saveg_write32(str->x);
    saveg_write32(str->y);
    saveg_write32(str->z);
    saveg_write32(str->angle);
    saveg_write_enum(str->sprite);
    saveg_write32(str->frame);
    saveg_write32(str->floorz);
    saveg_write32(str->ceilingz);
    saveg_write32(str->dropoffz);
    saveg_write32(str->radius);
    saveg_write32(str->height);
    saveg_write32(str->momx);
    saveg_write32(str->momy);
    saveg_write32(str->momz);
    saveg_write_enum(str->type);
    saveg_write32(str->tics);
    saveg_write32((int)(str->state - states));
    saveg_write32(str->flags);
    saveg_write32(str->flags2);
    saveg_write32(str->health);
    saveg_write32(str->movedir);
    saveg_write32(str->movecount);
    saveg_write32(P_ThingToIndex(str->target));
    saveg_write32(str->reactiontime);
    saveg_write32(str->threshold);
    saveg_write_bool(!!str->player);
    saveg_write_mapthing_t(&str->spawnpoint);
    saveg_write32(P_ThingToIndex(str->tracer));
    saveg_write32(P_ThingToIndex(str->lastenemy));
    saveg_write32(str->floatbob);
    saveg_write32(str->shadowoffset);
    saveg_write16(str->gear);
    saveg_write32(str->bloodsplats);
    saveg_write32(str->blood);
    saveg_write32(str->interpolate);
    saveg_write32(str->oldx);
    saveg_write32(str->oldy);
    saveg_write32(str->oldz);
    saveg_write32(str->oldangle);
    saveg_write32(str->nudge);
    saveg_write32(str->pitch);
    saveg_write32(str->id);
    saveg_write16(str->pursuecount);
    saveg_write16(str->strafecount);

    if (str->flags & MF_SHOOTABLE)
    {
        int i;

        for (i = 0; str->name[i] != '\0'; i++)
            saveg_write8(str->name[i]);

        for (; i < 100; i++)
            saveg_write8(0);
    }

    saveg_write32(str->madesound);
    saveg_write32(str->flags3);
    saveg_write32(str->inflicter);

    // [BH] For future features without breaking savegame compatibility
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
}

//
// bloodsplat_t
//
static void saveg_read_bloodsplat_t(bloodsplat_t *str)
{
    str->x = saveg_read32();
    str->y = saveg_read32();
    str->patch = saveg_read32();
    str->flip = saveg_read_bool();
    str->blood = saveg_read32();
}

static void saveg_write_bloodsplat_t(bloodsplat_t *str)
{
    saveg_write32(str->x);
    saveg_write32(str->y);
    saveg_write32(str->patch);
    saveg_write_bool(str->flip);
    saveg_write32(str->blood);
}

//
// ticcmd_t
//
static void saveg_read_ticcmd_t(ticcmd_t *str)
{
    str->forwardmove = saveg_read8();
    str->sidemove = saveg_read8();
    str->angleturn = saveg_read16();
    str->buttons = saveg_read8();
    str->lookdir = saveg_read32();
}

static void saveg_write_ticcmd_t(ticcmd_t *str)
{
    saveg_write8(str->forwardmove);
    saveg_write8(str->sidemove);
    saveg_write16(str->angleturn);
    saveg_write8(str->buttons);
    saveg_write32(str->lookdir);
}

//
// pspdef_t
//
static void saveg_read_pspdef_t(pspdef_t *str)
{
    int state;

    str->state = ((state = saveg_read32()) > 0 && state < NUMSTATES ? &states[state] : NULL);
    str->tics = saveg_read32();
    str->sx = saveg_read32();
    str->sy = saveg_read32();
}

static void saveg_write_pspdef_t(pspdef_t *str)
{
    saveg_write32(str->state ? (int)(str->state - states) : 0);
    saveg_write32(str->tics);
    saveg_write32(str->sx);
    saveg_write32(str->sy);
}

extern int  oldhealth;
extern int  cardsfound;

//
// player_t
//
static void saveg_read_player_t(void)
{
    viewplayer->playerstate = (playerstate_t)saveg_read_enum();
    saveg_read_ticcmd_t(&viewplayer->cmd);
    viewplayer->viewz = saveg_read32();
    viewplayer->viewheight = saveg_read32();
    viewplayer->deltaviewheight = saveg_read32();
    viewplayer->momx = saveg_read32();
    viewplayer->momy = saveg_read32();
    viewplayer->health = saveg_read32();
    oldhealth = saveg_read32();
    viewplayer->armorpoints = saveg_read32();
    viewplayer->armortype = (armortype_t)saveg_read_enum();

    for (int i = 0; i < NUMPOWERS; i++)
        viewplayer->powers[i] = saveg_read32();

    for (int i = 0; i < NUMCARDS; i++)
    {
        viewplayer->cards[i] = saveg_read32();
        cardsfound = MAX(cardsfound, viewplayer->cards[i]);
    }

    viewplayer->neededcard = saveg_read32();
    viewplayer->neededcardflash = saveg_read32();
    viewplayer->backpack = saveg_read_bool();
    viewplayer->readyweapon = (weapontype_t)saveg_read_enum();
    viewplayer->pendingweapon = (weapontype_t)saveg_read_enum();

    for (int i = 0; i < NUMWEAPONS; i++)
        viewplayer->weaponowned[i] = oldweaponsowned[i] = saveg_read32();

    for (int i = 0; i < NUMAMMO; i++)
        viewplayer->ammo[i] = saveg_read32();

    for (int i = 0; i < NUMAMMO; i++)
        viewplayer->maxammo[i] = saveg_read32();

    viewplayer->attackdown = saveg_read_bool();
    viewplayer->usedown = saveg_read_bool();
    viewplayer->cheats = saveg_read32();
    viewplayer->refire = saveg_read32();
    viewplayer->killcount = saveg_read32();
    viewplayer->itemcount = saveg_read32();
    viewplayer->secretcount = saveg_read32();
    viewplayer->damagecount = saveg_read32();
    viewplayer->bonuscount = saveg_read32();
    attacker = saveg_read32();
    viewplayer->extralight = saveg_read32();
    viewplayer->fixedcolormap = saveg_read32();
    saveg_read_pspdef_t(&viewplayer->psprites[ps_weapon]);
    saveg_read_pspdef_t(&viewplayer->psprites[ps_flash]);
    viewplayer->didsecret = saveg_read_bool();
    viewplayer->preferredshotgun = (weapontype_t)saveg_read_enum();
    viewplayer->fistorchainsaw = (weapontype_t)saveg_read_enum();
    viewplayer->invulnbeforechoppers = saveg_read_bool();
    viewplayer->chainsawbeforechoppers = saveg_read_bool();
    viewplayer->weaponbeforechoppers = (weapontype_t)saveg_read_enum();
    viewplayer->oldviewz = saveg_read32();
    viewplayer->lookdir = saveg_read32();
    viewplayer->oldlookdir = saveg_read32();
    viewplayer->recoil = saveg_read32();
    viewplayer->oldrecoil = saveg_read32();
    viewplayer->jumptics = saveg_read32();

    if (!mouselook)
    {
        viewplayer->lookdir = 0;
        viewplayer->oldlookdir = 0;
        viewplayer->recoil = 0;
        viewplayer->oldrecoil = 0;
    }

    viewplayer->damageinflicted = saveg_read32();
    viewplayer->damagereceived = saveg_read32();
    viewplayer->cheated = saveg_read32();
    viewplayer->shotshit = saveg_read32();
    viewplayer->shotsfired = saveg_read32();
    viewplayer->deaths = saveg_read32();

    for (int i = 0; i < NUMMOBJTYPES; i++)
        viewplayer->mobjcount[i] = saveg_read32();

    viewplayer->distancetraveled = saveg_read32();
    viewplayer->gamessaved = saveg_read32();
    viewplayer->itemspickedup_ammo_bullets = saveg_read32();
    viewplayer->itemspickedup_ammo_cells = saveg_read32();
    viewplayer->itemspickedup_ammo_rockets = saveg_read32();
    viewplayer->itemspickedup_ammo_shells = saveg_read32();
    viewplayer->itemspickedup_armor = saveg_read32();
    viewplayer->itemspickedup_health = saveg_read32();
    viewplayer->suicides = saveg_read32();

    viewplayer->bounce = saveg_read32();
    viewplayer->bouncemax = saveg_read32();

    // [BH] For future features without breaking savegame compatibility
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
    saveg_read32();
}

static void saveg_write_player_t(void)
{
    saveg_write_enum(viewplayer->playerstate);
    saveg_write_ticcmd_t(&viewplayer->cmd);
    saveg_write32(viewplayer->viewz);
    saveg_write32(viewplayer->viewheight);
    saveg_write32(viewplayer->deltaviewheight);
    saveg_write32(viewplayer->momx);
    saveg_write32(viewplayer->momy);
    saveg_write32(viewplayer->health);
    saveg_write32(oldhealth);
    saveg_write32(viewplayer->armorpoints);
    saveg_write_enum(viewplayer->armortype);

    for (int i = 0; i < NUMPOWERS; i++)
        saveg_write32(viewplayer->powers[i]);

    for (int i = 0; i < NUMCARDS; i++)
        saveg_write32(viewplayer->cards[i]);

    saveg_write32(viewplayer->neededcard);
    saveg_write32(viewplayer->neededcardflash);
    saveg_write_bool(viewplayer->backpack);
    saveg_write_enum(viewplayer->readyweapon);
    saveg_write_enum(viewplayer->pendingweapon);

    for (int i = 0; i < NUMWEAPONS; i++)
        saveg_write32(viewplayer->weaponowned[i]);

    for (int i = 0; i < NUMAMMO; i++)
        saveg_write32(viewplayer->ammo[i]);

    for (int i = 0; i < NUMAMMO; i++)
        saveg_write32(viewplayer->maxammo[i]);

    saveg_write_bool(viewplayer->attackdown);
    saveg_write_bool(viewplayer->usedown);
    saveg_write32(viewplayer->cheats);
    saveg_write32(viewplayer->refire);
    saveg_write32(viewplayer->killcount);
    saveg_write32(viewplayer->itemcount);
    saveg_write32(viewplayer->secretcount);
    saveg_write32(viewplayer->damagecount);
    saveg_write32(viewplayer->bonuscount);
    saveg_write32(P_ThingToIndex(viewplayer->attacker));
    saveg_write32(viewplayer->extralight);
    saveg_write32(viewplayer->fixedcolormap);
    saveg_write_pspdef_t(&viewplayer->psprites[ps_weapon]);
    saveg_write_pspdef_t(&viewplayer->psprites[ps_flash]);
    saveg_write_bool(viewplayer->didsecret);
    saveg_write_enum(viewplayer->preferredshotgun);
    saveg_write32(viewplayer->fistorchainsaw);
    saveg_write_bool(viewplayer->invulnbeforechoppers);
    saveg_write_bool(viewplayer->chainsawbeforechoppers);
    saveg_write_enum(viewplayer->weaponbeforechoppers);
    saveg_write32(viewplayer->oldviewz);
    saveg_write32(viewplayer->lookdir);
    saveg_write32(viewplayer->oldlookdir);
    saveg_write32(viewplayer->recoil);
    saveg_write32(viewplayer->oldrecoil);
    saveg_write32(viewplayer->jumptics);
    saveg_write32(viewplayer->damageinflicted);
    saveg_write32(viewplayer->damagereceived);
    saveg_write32(viewplayer->cheated);
    saveg_write32(viewplayer->shotshit);
    saveg_write32(viewplayer->shotsfired);
    saveg_write32(viewplayer->deaths);

    for (int i = 0; i < NUMMOBJTYPES; i++)
        saveg_write32(viewplayer->mobjcount[i]);

    saveg_write32(viewplayer->distancetraveled);
    saveg_write32(viewplayer->gamessaved);
    saveg_write32(viewplayer->itemspickedup_ammo_bullets);
    saveg_write32(viewplayer->itemspickedup_ammo_cells);
    saveg_write32(viewplayer->itemspickedup_ammo_rockets);
    saveg_write32(viewplayer->itemspickedup_ammo_shells);
    saveg_write32(viewplayer->itemspickedup_armor);
    saveg_write32(viewplayer->itemspickedup_health);
    saveg_write32(viewplayer->suicides);

    saveg_write32(viewplayer->bounce);
    saveg_write32(viewplayer->bouncemax);

    // [BH] For future features without breaking savegame compatibility
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
    saveg_write32(0);
}

//
// ceiling_t
//
static void saveg_read_ceiling_t(ceiling_t *str)
{
    str->type = (ceiling_e)saveg_read_enum();
    str->sector = sectors + saveg_read32();
    str->bottomheight = saveg_read32();
    str->topheight = saveg_read32();
    str->speed = saveg_read32();
    str->oldspeed = saveg_read32();
    str->crush = saveg_read_bool();
    str->newspecial = saveg_read32();
    str->texture = saveg_read16();
    str->direction = saveg_read32();
    str->tag = saveg_read32();
    str->olddirection = saveg_read32();
}

static void saveg_write_ceiling_t(ceiling_t *str)
{
    saveg_write_enum(str->type);
    saveg_write32(str->sector->id);
    saveg_write32(str->bottomheight);
    saveg_write32(str->topheight);
    saveg_write32(str->speed);
    saveg_write32(str->oldspeed);
    saveg_write_bool(str->crush);
    saveg_write32(str->newspecial);
    saveg_write16(str->texture);
    saveg_write32(str->direction);
    saveg_write32(str->tag);
    saveg_write32(str->olddirection);
}

//
// vldoor_t
//
static void saveg_read_vldoor_t(vldoor_t *str)
{
    int line;

    str->type = (vldoor_e)saveg_read_enum();
    str->sector = sectors + saveg_read32();
    str->topheight = saveg_read32();
    str->speed = saveg_read32();
    str->direction = saveg_read32();
    str->topwait = saveg_read32();
    str->topcountdown = saveg_read32();
    str->line = ((line = saveg_read32()) >= 0 ? lines + line : NULL);
    str->lighttag = saveg_read32();
}

static void saveg_write_vldoor_t(vldoor_t *str)
{
    saveg_write_enum(str->type);
    saveg_write32(str->sector->id);
    saveg_write32(str->topheight);
    saveg_write32(str->speed);
    saveg_write32(str->direction);
    saveg_write32(str->topwait);
    saveg_write32(str->topcountdown);
    saveg_write32(str->line ? str->line->id : -1);
    saveg_write32(str->lighttag);
}

//
// floormove_t
//
static void saveg_read_floormove_t(floormove_t *str)
{
    str->type = (floor_e)saveg_read_enum();
    str->crush = saveg_read_bool();
    str->sector = sectors + saveg_read32();
    str->direction = saveg_read32();
    str->newspecial = saveg_read32();
    str->texture = saveg_read16();
    str->floordestheight = saveg_read32();
    str->speed = saveg_read32();
    str->stopsound = saveg_read32();
}

static void saveg_write_floormove_t(floormove_t *str)
{
    saveg_write_enum(str->type);
    saveg_write_bool(str->crush);
    saveg_write32(str->sector->id);
    saveg_write32(str->direction);
    saveg_write32(str->newspecial);
    saveg_write16(str->texture);
    saveg_write32(str->floordestheight);
    saveg_write32(str->speed);
    saveg_write32(str->stopsound);
}

//
// plat_t
//
static void saveg_read_plat_t(plat_t *str)
{
    str->thinker.function = (saveg_read_bool() ? &T_PlatRaise : NULL);
    str->sector = sectors + saveg_read32();
    str->speed = saveg_read32();
    str->low = saveg_read32();
    str->high = saveg_read32();
    str->wait = saveg_read32();
    str->count = saveg_read32();
    str->status = (plat_e)saveg_read_enum();
    str->oldstatus = (plat_e)saveg_read_enum();
    str->crush = saveg_read_bool();
    str->tag = saveg_read32();
    str->type = (plattype_e)saveg_read_enum();
}

static void saveg_write_plat_t(plat_t *str)
{
    saveg_write_bool(!!str->thinker.function);
    saveg_write32(str->sector->id);
    saveg_write32(str->speed);
    saveg_write32(str->low);
    saveg_write32(str->high);
    saveg_write32(str->wait);
    saveg_write32(str->count);
    saveg_write_enum(str->status);
    saveg_write_enum(str->oldstatus);
    saveg_write_bool(str->crush);
    saveg_write32(str->tag);
    saveg_write_enum(str->type);
}

//
// lightflash_t
//
static void saveg_read_lightflash_t(lightflash_t *str)
{
    str->sector = sectors + saveg_read32();
    str->count = saveg_read32();
    str->maxlight = saveg_read32();
    str->minlight = saveg_read32();
    str->maxtime = saveg_read32();
    str->mintime = saveg_read32();
}

static void saveg_write_lightflash_t(lightflash_t *str)
{
    saveg_write32(str->sector->id);
    saveg_write32(str->count);
    saveg_write32(str->maxlight);
    saveg_write32(str->minlight);
    saveg_write32(str->maxtime);
    saveg_write32(str->mintime);
}

//
// strobe_t
//
static void saveg_read_strobe_t(strobe_t *str)
{
    str->sector = sectors + saveg_read32();
    str->count = saveg_read32();
    str->minlight = saveg_read32();
    str->maxlight = saveg_read32();
    str->darktime = saveg_read32();
    str->brighttime = saveg_read32();
}

static void saveg_write_strobe_t(strobe_t *str)
{
    saveg_write32(str->sector->id);
    saveg_write32(str->count);
    saveg_write32(str->minlight);
    saveg_write32(str->maxlight);
    saveg_write32(str->darktime);
    saveg_write32(str->brighttime);
}

//
// glow_t
//
static void saveg_read_glow_t(glow_t *str)
{
    str->sector = sectors + saveg_read32();
    str->minlight = saveg_read32();
    str->maxlight = saveg_read32();
    str->direction = saveg_read32();
}

static void saveg_write_glow_t(glow_t *str)
{
    saveg_write32(str->sector->id);
    saveg_write32(str->minlight);
    saveg_write32(str->maxlight);
    saveg_write32(str->direction);
}

static void saveg_read_fireflicker_t(fireflicker_t *str)
{
    str->sector = sectors + saveg_read32();
    str->count = saveg_read32();
    str->minlight = saveg_read32();
    str->maxlight = saveg_read32();
}

static void saveg_write_fireflicker_t(fireflicker_t *str)
{
    saveg_write32(str->sector->id);
    saveg_write32(str->count);
    saveg_write32(str->minlight);
    saveg_write32(str->maxlight);
}

static void saveg_read_elevator_t(elevator_t *str)
{
    str->type = (elevator_e)saveg_read_enum();
    str->sector = sectors + saveg_read32();
    str->direction = saveg_read32();
    str->floordestheight = saveg_read32();
    str->ceilingdestheight = saveg_read32();
    str->speed = saveg_read32();
    //str->stopsound = saveg_read32();
}

static void saveg_write_elevator_t(elevator_t *str)
{
    saveg_write_enum(str->type);
    saveg_write32(str->sector->id);
    saveg_write32(str->direction);
    saveg_write32(str->floordestheight);
    saveg_write32(str->ceilingdestheight);
    saveg_write32(str->speed);
    //saveg_write32(str->stopsound);
}

static void saveg_read_scroll_t(scroll_t *str)
{
    str->dx = saveg_read32();
    str->dy = saveg_read32();
    str->affectee = saveg_read32();
    str->control = saveg_read32();
    str->last_height = saveg_read32();
    str->vdx = saveg_read32();
    str->vdy = saveg_read32();
    str->accel = saveg_read32();
    str->type = saveg_read_enum();
}

static void saveg_write_scroll_t(scroll_t *str)
{
    saveg_write32(str->dx);
    saveg_write32(str->dy);
    saveg_write32(str->affectee);
    saveg_write32(str->control);
    saveg_write32(str->last_height);
    saveg_write32(str->vdx);
    saveg_write32(str->vdy);
    saveg_write32(str->accel);
    saveg_write_enum(str->type);
}

static void saveg_read_pusher_t(pusher_t *str)
{
    str->type = saveg_read_enum();
    str->x_mag = saveg_read32();
    str->y_mag = saveg_read32();
    str->magnitude = saveg_read32();
    str->radius = saveg_read32();
    str->x = saveg_read32();
    str->y = saveg_read32();
    str->affectee = saveg_read32();
}

static void saveg_write_pusher_t(pusher_t *str)
{
    saveg_write_enum(str->type);
    saveg_write32(str->x_mag);
    saveg_write32(str->y_mag);
    saveg_write32(str->magnitude);
    saveg_write32(str->radius);
    saveg_write32(str->x);
    saveg_write32(str->y);
    saveg_write32(str->affectee);
}

static void saveg_read_button_t(button_t *str)
{
    int line = saveg_read32();

    str->line = (line >= 0 ? lines + line : NULL);
    str->where = (bwhere_e)saveg_read_enum();
    str->btexture = saveg_read32();
    str->btimer = saveg_read32();
}

static void saveg_write_button_t(button_t *str)
{
    saveg_write32((str->line ? str->line->id : -1));
    saveg_write_enum(str->where);
    saveg_write32(str->btexture);
    saveg_write32(str->btimer);
}

//
// Write the header for a savegame
//
void P_WriteSaveGameHeader(char *description)
{
    char    name[VERSIONSIZE];
    int     i;

    for (i = 0; description[i] != '\0'; i++)
        saveg_write8(description[i]);

    for (; i < SAVESTRINGSIZE; i++)
        saveg_write8(0);

    memset(name, 0, sizeof(name));
    strcpy(name, PACKAGE_SAVEGAMEVERSIONSTRING);

    for (i = 0; i < VERSIONSIZE; i++)
        saveg_write8(name[i]);

    saveg_write8(gameskill);
    saveg_write8(gameepisode);

    if (M_StringCompare(mapnum, "E1M4B"))
        saveg_write8(10);
    else if (M_StringCompare(mapnum, "E1M8B"))
        saveg_write8(11);
    else
        saveg_write8(gamemap);

    saveg_write8(gamemission);
    saveg_write8((leveltime >> 16) & 0xFF);
    saveg_write8((leveltime >> 8) & 0xFF);
    saveg_write8(leveltime & 0xFF);
}

//
// Read the header for a savegame
//
dboolean P_ReadSaveGameHeader(char *description)
{
    byte    a, b, c;
    char    vcheck[VERSIONSIZE];
    char    read_vcheck[VERSIONSIZE];

    for (int i = 0; i < SAVESTRINGSIZE; i++)
        description[i] = saveg_read8();

    for (int i = 0; i < VERSIONSIZE; i++)
        read_vcheck[i] = saveg_read8();

    memset(vcheck, 0, sizeof(vcheck));
    strcpy(vcheck, PACKAGE_SAVEGAMEVERSIONSTRING);

    if (!M_StringCompare(read_vcheck, vcheck))
    {
        menuactive = false;
        C_ShowConsole();
        C_Warning(1, "This savegame is incompatible with <i>" PACKAGE_NAMEANDVERSIONSTRING "</i>.");
        return false;   // bad version
    }

    gameskill = (skill_t)saveg_read8();
    gameepisode = saveg_read8();
    gamemap = saveg_read8();

    if (gamemode != commercial)
    {
        if (gamemap == 10)
        {
            gamemap = 4;
            M_StringCopy(speciallumpname, "E1M4B", sizeof(speciallumpname));
        }
        else if (gamemap == 11)
        {
            gamemap = 8;
            M_StringCopy(speciallumpname, "E1M8B", sizeof(speciallumpname));
        }
    }

    saveg_read8();      // gamemission

    // get the times
    a = saveg_read8();
    b = saveg_read8();
    c = saveg_read8();
    leveltime = (a << 16) + (b << 8) + c;

    return true;
}

//
// Read the end of file marker. Returns true if read successfully.
//
dboolean P_ReadSaveGameEOF(void)
{
    return (saveg_read8() == SAVEGAME_EOF);
}

//
// Write the end of file marker
//
void P_WriteSaveGameEOF(void)
{
    saveg_write8(SAVEGAME_EOF);
}

//
// P_ArchivePlayer
//
void P_ArchivePlayer(void)
{
    saveg_write_player_t();
}

//
// P_UnArchivePlayer
//
void P_UnArchivePlayer(void)
{
    P_InitCards();
    saveg_read_player_t();
}

//
// P_ArchiveWorld
//
void P_ArchiveWorld(void)
{
    sector_t    *sector = sectors;
    line_t      *line = lines;

    // do sectors
    for (int i = 0; i < numsectors; i++, sector++)
    {
        saveg_write16(sector->floorheight >> FRACBITS);
        saveg_write16(sector->ceilingheight >> FRACBITS);
        saveg_write16(sector->floorpic);
        saveg_write16(sector->ceilingpic);
        saveg_write16(sector->lightlevel);
        saveg_write16(sector->special);
        saveg_write16(sector->tag);
        saveg_write32(P_ThingToIndex(sector->soundtarget));
    }

    // do lines
    for (int i = 0; i < numlines; i++, line++)
    {
        saveg_write16(line->flags);
        saveg_write16(line->special);
        saveg_write16(line->tag);

        for (int j = 0; j < 2; j++)
        {
            side_t  *side;

            if (line->sidenum[j] == NO_INDEX)
                continue;

            side = sides + line->sidenum[j];

            saveg_write16(side->textureoffset >> FRACBITS);
            saveg_write16(side->rowoffset >> FRACBITS);
            saveg_write16(side->toptexture);
            saveg_write16(side->bottomtexture);
            saveg_write16(side->midtexture);
            saveg_write_bool(side->missingtoptexture);
            saveg_write_bool(side->missingbottomtexture);
            saveg_write_bool(side->missingmidtexture);
        }
    }
}

//
// P_UnArchiveWorld
//
void P_UnArchiveWorld(void)
{
    sector_t    *sector = sectors;
    line_t      *line = lines;

    // do sectors
    for (int i = 0; i < numsectors; i++, sector++)
    {
        sector->floorheight = saveg_read16() << FRACBITS;
        sector->ceilingheight = saveg_read16() << FRACBITS;
        sector->floorpic = saveg_read16();
        sector->terraintype = terraintypes[sector->floorpic];
        sector->ceilingpic = saveg_read16();
        sector->lightlevel = sector->oldlightlevel = saveg_read16();
        sector->special = saveg_read16();
        sector->tag = saveg_read16();
        sector->ceilingdata = NULL;
        sector->floordata = NULL;
        sector->lightingdata = NULL;
        soundtargets[MIN(i, TARGETLIMIT - 1)] = saveg_read32();
    }

    // do lines
    for (int i = 0; i < numlines; i++, line++)
    {
        line->flags = saveg_read16();
        line->special = saveg_read16();
        line->tag = saveg_read16();

        for (int j = 0; j < 2; j++)
        {
            side_t  *side;

            if (line->sidenum[j] == NO_INDEX)
                continue;

            side = sides + line->sidenum[j];

            side->textureoffset = saveg_read16() << FRACBITS;
            side->rowoffset = saveg_read16() << FRACBITS;
            side->toptexture = saveg_read16();
            side->bottomtexture = saveg_read16();
            side->midtexture = saveg_read16();
            side->missingtoptexture = saveg_read_bool();
            side->missingbottomtexture = saveg_read_bool();
            side->missingmidtexture = saveg_read_bool();
        }
    }
}

//
// Thinkers
//

//
// P_ArchiveThinkers
//
void P_ArchiveThinkers(void)
{
    // save off the current thinkers
    for (thinker_t *th = thinkers[th_mobj].cnext; th != &thinkers[th_mobj]; th = th->cnext)
    {
        saveg_write8(tc_mobj);
        saveg_write_mobj_t((mobj_t *)th);
    }

    // save off the bloodsplats
    for (int i = 0; i < numsectors; i++)
        for (bloodsplat_t *splat = sectors[i].splatlist; splat; splat = splat->snext)
        {
            saveg_write8(tc_bloodsplat);
            saveg_write_bloodsplat_t(splat);
        }

    // add a terminating marker
    saveg_write8(tc_end);
}

//
// killough 11/98
//
// Same as P_SetTarget() in p_tick.c, except that the target is nullified
// first, so that no old target's reference count is decreased (when loading
// savegames, old targets are indices, not really pointers to targets).
//
static void P_SetNewTarget(mobj_t **mop, mobj_t *targ)
{
    *mop = NULL;
    P_SetTarget(mop, targ);
}

//
// P_UnArchiveThinkers
//
void P_UnArchiveThinkers(void)
{
    thinker_t   *currentthinker = thinkers[th_all].next;

    // remove all the current thinkers
    while (currentthinker != &thinkers[th_all])
    {
        thinker_t   *next = currentthinker->next;

        if (currentthinker->function == &P_MobjThinker || currentthinker->function == &MusInfoThinker)
        {
            P_RemoveMobj((mobj_t *)currentthinker);
            P_RemoveThinkerDelayed(currentthinker);
        }
        else
            Z_Free(currentthinker);

        currentthinker = next;
    }

    P_InitThinkers();

    // remove all bloodsplats
    for (int i = 0; i < numsectors; i++)
    {
        bloodsplat_t    *splat = sectors[i].splatlist;

        while (splat)
        {
            bloodsplat_t    *next = splat->snext;

            P_UnsetBloodSplatPosition(splat);
            splat = next;
        }
    }

    r_bloodsplats_total = 0;
    thingindex = 0;

    // read in saved thinkers
    while (true)
    {
        byte    tclass = saveg_read8();

        switch (tclass)
        {
            case tc_end:
                return;         // end of list

            case tc_mobj:
            {
                mobj_t  *mobj = Z_Calloc(1, sizeof(*mobj), PU_LEVEL, NULL);

                saveg_read_mobj_t(mobj);

                mobj->info = &mobjinfo[mobj->type];
                P_SetThingPosition(mobj);

                mobj->thinker.function = (mobj->type == MT_MUSICSOURCE ? &MusInfoThinker : &P_MobjThinker);
                P_AddThinker(&mobj->thinker);
                mobj->colfunc = mobj->info->colfunc;
                mobj->altcolfunc = mobj->info->altcolfunc;
                P_SetShadowColumnFunction(mobj);
                thingindex = MIN(thingindex + 1, TARGETLIMIT - 1);
                break;
            }

            case tc_bloodsplat:
            {
                bloodsplat_t    *splat = calloc(1, sizeof(*splat));

                saveg_read_bloodsplat_t(splat);

                if (r_bloodsplats_total < r_bloodsplats_max)
                {
                    splat->width = spritewidth[splat->patch];
                    splat->sector = R_PointInSubsector(splat->x, splat->y)->sector;
                    P_SetBloodSplatPosition(splat);
                    splat->colfunc = (splat->blood == FUZZYBLOOD ? fuzzcolfunc : bloodsplatcolfunc);
                    r_bloodsplats_total++;
                }

                break;
            }

            default:
                I_Error("%s is invalid.", savename);
        }
    }
}

void P_RestoreTargets(void)
{
    sector_t    *sec = sectors;
    int         targetlimit = MIN(numsectors, TARGETLIMIT - 1);

    P_SetNewTarget(&viewplayer->attacker, P_IndexToThing(attacker));

    for (int i = 0; i < targetlimit; i++, sec++)
        P_SetNewTarget(&sec->soundtarget, P_IndexToThing(soundtargets[i]));

    thingindex = 0;

    for (thinker_t *th = thinkers[th_mobj].cnext; th != &thinkers[th_mobj]; th = th->cnext)
    {
        mobj_t  *mo = (mobj_t *)th;

        P_SetNewTarget(&mo->target, P_IndexToThing(targets[thingindex]));
        P_SetNewTarget(&mo->tracer, P_IndexToThing(tracers[thingindex]));
        P_SetNewTarget(&mo->lastenemy, P_IndexToThing(lastenemies[thingindex]));
        thingindex = MIN(thingindex + 1, TARGETLIMIT - 1);
    }
}

//
// P_RemoveCorruptMobjs
//
void P_RemoveCorruptMobjs(void)
{
    for (thinker_t *th = thinkers[th_mobj].cnext; th != &thinkers[th_mobj]; th = th->cnext)
    {
        mobj_t  *mo = (mobj_t *)th;

        if (!mo->state && mo->info->spawnstate != S_NULL)
            P_RemoveMobj(mo);
    }
}

//
// P_ArchiveSpecials
//
void P_ArchiveSpecials(void)
{
    int         i = maxbuttons;
    button_t    *button = buttonlist;

    // save off the current thinkers
    for (thinker_t *th = thinkers[th_misc].cnext; th != &thinkers[th_misc]; th = th->cnext)
    {
        if (!th->function)
        {
            dboolean    done_one = false;

            for (ceilinglist_t *ceilinglist = activeceilings; ceilinglist; ceilinglist = ceilinglist->next)
                if (ceilinglist->ceiling == (ceiling_t *)th)
                {
                    saveg_write8(tc_ceiling);
                    saveg_write_ceiling_t((ceiling_t *)th);
                    done_one = true;
                    break;
                }

            // [jeff-d] save height of moving platforms
            for (platlist_t *platlist = activeplats; platlist; platlist = platlist->next)
                if (platlist->plat == (plat_t *)th)
                {
                    saveg_write8(tc_plat);
                    saveg_write_plat_t((plat_t *)th);
                    done_one = true;
                    break;
                }

            if (done_one)
                continue;
        }

        if (th->function == &T_MoveCeiling)
        {
            saveg_write8(tc_ceiling);
            saveg_write_ceiling_t((ceiling_t *)th);
            continue;
        }

        if (th->function == &T_VerticalDoor)
        {
            saveg_write8(tc_door);
            saveg_write_vldoor_t((vldoor_t *)th);
            continue;
        }

        if (th->function == &T_MoveFloor)
        {
            saveg_write8(tc_floor);
            saveg_write_floormove_t((floormove_t *)th);
            continue;
        }

        if (th->function == &T_PlatRaise)
        {
            saveg_write8(tc_plat);
            saveg_write_plat_t((plat_t *)th);
            continue;
        }

        if (th->function == &T_LightFlash)
        {
            saveg_write8(tc_flash);
            saveg_write_lightflash_t((lightflash_t *)th);
            continue;
        }

        if (th->function == &T_StrobeFlash)
        {
            saveg_write8(tc_strobe);
            saveg_write_strobe_t((strobe_t *)th);
            continue;
        }

        if (th->function == &T_Glow)
        {
            saveg_write8(tc_glow);
            saveg_write_glow_t((glow_t *)th);
            continue;
        }

        if (th->function == &T_FireFlicker)
        {
            saveg_write8(tc_fireflicker);
            saveg_write_fireflicker_t((fireflicker_t *)th);
            continue;
        }

        if (th->function == &T_MoveElevator)
        {
            saveg_write8(tc_elevator);
            saveg_write_elevator_t((elevator_t *)th);
            continue;
        }

        if (th->function == &T_Scroll)
        {
            saveg_write8(tc_scroll);
            saveg_write_scroll_t((scroll_t *)th);
            continue;
        }

        if (th->function == &T_Pusher)
        {
            saveg_write8(tc_pusher);
            saveg_write_pusher_t((pusher_t *)th);
            continue;
        }
    }

    do
    {
        if (button->btimer)
        {
            saveg_write8(tc_button);
            saveg_write_button_t(button);
        }

        button++;
    } while (--i);

    // add a terminating marker
    saveg_write8(tc_endspecials);
}

//
// P_UnArchiveSpecials
//
void P_UnArchiveSpecials(void)
{
    // read in saved thinkers
    while (true)
    {
        byte    tclass = saveg_read8();

        switch (tclass)
        {
            case tc_endspecials:
                // end of list
                return;

            case tc_ceiling:
            {
                ceiling_t   *ceiling = Z_Malloc(sizeof(*ceiling), PU_LEVEL, NULL);

                saveg_read_ceiling_t(ceiling);
                ceiling->sector->ceilingdata = ceiling;
                ceiling->thinker.function = &T_MoveCeiling;
                P_AddThinker(&ceiling->thinker);
                P_AddActiveCeiling(ceiling);
                break;
            }

            case tc_door:
            {
                vldoor_t    *door = Z_Malloc(sizeof(*door), PU_LEVEL, NULL);

                saveg_read_vldoor_t(door);
                door->sector->ceilingdata = door;
                door->thinker.function = &T_VerticalDoor;
                P_AddThinker(&door->thinker);
                break;
            }

            case tc_floor:
            {
                floormove_t *floor = Z_Malloc(sizeof(*floor), PU_LEVEL, NULL);

                saveg_read_floormove_t(floor);
                floor->sector->floordata = floor;
                floor->thinker.function = &T_MoveFloor;
                P_AddThinker(&floor->thinker);
                break;
            }

            case tc_plat:
            {
                plat_t  *plat = Z_Malloc(sizeof(*plat), PU_LEVEL, NULL);

                saveg_read_plat_t(plat);
                plat->sector->floordata = plat;
                plat->thinker.function = &T_PlatRaise;
                P_AddThinker(&plat->thinker);
                P_AddActivePlat(plat);
                break;
            }

            case tc_flash:
            {
                lightflash_t    *flash = Z_Malloc(sizeof(*flash), PU_LEVEL, NULL);

                saveg_read_lightflash_t(flash);
                flash->thinker.function = &T_LightFlash;
                P_AddThinker(&flash->thinker);
                break;
            }

            case tc_strobe:
            {
                strobe_t    *strobe = Z_Malloc(sizeof(*strobe), PU_LEVEL, NULL);

                saveg_read_strobe_t(strobe);
                strobe->thinker.function = &T_StrobeFlash;
                P_AddThinker(&strobe->thinker);
                break;
            }

            case tc_glow:
            {
                glow_t  *glow = Z_Malloc(sizeof(*glow), PU_LEVEL, NULL);

                saveg_read_glow_t(glow);
                glow->thinker.function = &T_Glow;
                P_AddThinker(&glow->thinker);
                break;
            }

            case tc_fireflicker:
            {
                fireflicker_t   *fireflicker = Z_Malloc(sizeof(*fireflicker), PU_LEVEL, NULL);

                saveg_read_fireflicker_t(fireflicker);
                fireflicker->thinker.function = &T_FireFlicker;
                P_AddThinker(&fireflicker->thinker);
                break;
            }

            case tc_elevator:
            {
                elevator_t  *elevator = Z_Malloc(sizeof(*elevator), PU_LEVEL, NULL);

                saveg_read_elevator_t(elevator);
                elevator->sector->ceilingdata = elevator;
                elevator->thinker.function = &T_MoveElevator;
                P_AddThinker(&elevator->thinker);
                break;
            }

            case tc_scroll:
            {
                scroll_t    *scroll = Z_Malloc(sizeof(*scroll), PU_LEVEL, NULL);

                saveg_read_scroll_t(scroll);
                scroll->thinker.function = &T_Scroll;
                P_AddThinker(&scroll->thinker);
                break;
            }

            case tc_pusher:
            {
                pusher_t    *pusher = Z_Malloc(sizeof(*pusher), PU_LEVEL, NULL);

                saveg_read_pusher_t(pusher);
                pusher->thinker.function = &T_Pusher;
                pusher->source = P_GetPushThing(pusher->affectee);
                P_AddThinker(&pusher->thinker);
                break;
            }

            case tc_button:
            {
                button_t    *button = Z_Malloc(sizeof(*button), PU_LEVEL, NULL);

                saveg_read_button_t(button);
                P_StartButton(button->line, button->where, button->btexture, button->btimer);
                break;
            }

            default:
                I_Error("%s is invalid.", savename);
        }
    }
}

//
// P_ArchiveMap
//
void P_ArchiveMap(void)
{
    saveg_write_bool(automapactive);
    saveg_write32(markpointnum);

    if (markpointnum)
        for (int i = 0; i < markpointnum; i++)
        {
            saveg_write32(markpoints[i].x);
            saveg_write32(markpoints[i].y);
        }

    saveg_write32(pathpointnum);

    if (pathpointnum)
        for (int i = 0; i < pathpointnum; i++)
        {
            saveg_write32(pathpoints[i].x);
            saveg_write32(pathpoints[i].y);
        }
}

//
// P_UnArchiveMap
//
void P_UnArchiveMap(void)
{
    if ((automapactive = saveg_read_bool()) || mapwindow)
        AM_Start(automapactive);

    if ((markpointnum = saveg_read32()))
    {
        markpoints = I_Realloc(markpoints, (markpointnum_max = markpointnum * 2) * sizeof(*markpoints));

        for (int i = 0; i < markpointnum; i++)
        {
            markpoints[i].x = saveg_read32();
            markpoints[i].y = saveg_read32();
        }
    }

    if ((pathpointnum = saveg_read32()))
    {
        pathpoints = I_Realloc(pathpoints, (pathpointnum_max = pathpointnum * 2) * sizeof(*pathpoints));

        for (int i = 0; i < pathpointnum; i++)
        {
            pathpoints[i].x = saveg_read32();
            pathpoints[i].y = saveg_read32();
        }
    }
}
