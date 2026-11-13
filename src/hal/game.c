/* game.c - what the port knows about UGH.EXE: the hooks that tell the Amiga sound what the DOS
 * game plays.
 *
 * The DOS game's music goes through a few far routines of its sound module (segment 0988 at run
 * time), called with the tune's data as a far pointer.  The hooks watch those entries (gen_watch)
 * and start the Amiga version's tune for the same moment, the way the Amiga game does it (TFMX
 * subsong, then a fade to the volume it uses there):
 *
 *   DOS tune (segment)        moment                    Amiga
 *   3747                      title                     subsong 0, full volume
 *   3554                      playing a level           the level's subsong (4, 5 or 6), half volume
 *   3739                      level start / next life   subsong 3 (the level-start jingle)
 *   337B                      game over                 subsong 2
 *   3389                      high score / the end      subsong 1
 *   (stop)                                              fade out
 *
 * The level's subsong comes from the Amiga game's level table (the word at +24h of each of the 69
 * levels, in play order); the DOS game keeps the level's index at 5D19:261C.
 * The sound effects: the DOS game has none of the Amiga version's (a few AdLib ones at most).
 * The Amiga game plays them as notes 14MM3300 (MM: the effect, 0..0E, on Paula channel 3) when an
 * object enters a state; the DOS game keeps the same objects and state tables (its descriptors
 * are the Amiga ones with longs turned into words), so each effect hooks the DOS handler of the
 * same state (game code segment 024B, BX = the object's slot; see docs/PLAN.md).
 *
 * UGH_SNDLOG=1 prints every call. */
#include "hal.h"
#include "../gen/gen.h"
#include "../audio/amiga_audio.h"
#include "../audio/tfmx.h"
#include <stdio.h>
#include <stdlib.h>

#define DGROUP 0x5D19u
#define LEVEL_INDEX 0x261Cu

static int g_log;

static const uint8_t g_level_song[69] = {
    4, 5, 5, 5, 5, 4, 6, 6, 6, 6, 6, 5, 6, 6, 4, 5, 4, 6, 4, 6, 5, 5, 4, 4, 5, 4, 4, 6, 5, 4,
    6, 5, 4, 6, 4, 4, 4, 4, 6, 5, 5, 4, 5, 6, 6, 5, 4, 5, 4, 5, 6, 4, 6, 4, 4, 6, 5, 5, 4, 6,
    4, 4, 6, 5, 6, 5, 4, 6, 5
};

int game_identify(const uint8_t *image, size_t len)
{
    (void)image; (void)len;
    return 1;
}

/* the word at SS:SP+off (a far call's arguments start at +4) */
static uint16_t arg(Cpu *c, unsigned off)
{
    return rws(c->s[SS], (uint16_t)(c->r[SP] + off));
}

static void log_call(Cpu *c, const char *what)
{
    if (g_log)
        fprintf(stderr, "snd: frame %lu %s from %04x:%04x args %04x %04x %04x %04x level %u\n", hal_frame_count(), what,
                arg(c, 2), arg(c, 0), arg(c, 4), arg(c, 6), arg(c, 8), arg(c, 10), rws(DGROUP, LEVEL_INDEX));
}

/* the tune the game is playing now (-1: none), kept so that an Amiga disk found later (Locate
 * Amiga disk...) starts it at once instead of at the game's next tune */
static int g_tune = -1, g_tune_speed, g_tune_volume;

static void amiga_tune(int song, int fade_speed, int volume)
{
    g_tune = song;
    g_tune_speed = fade_speed;
    g_tune_volume = volume;
    amiga_audio_song(song);
    if (amiga_audio_ready()) tfmx_fade(fade_speed, volume);
}

void game_amiga_resume(void)
{
    if (amiga_audio_ready() && g_tune >= 0 && !tfmx_playing()) {
        amiga_audio_song(g_tune);
        tfmx_fade(g_tune_speed, g_tune_volume);
    }
}

/* the DOS tune, by its data segment: the Amiga tune for the same moment */
static void play(uint16_t seg)
{
    switch (seg) {
    case 0x3747: amiga_tune(0, 0, 0x40); break;
    case 0x3554: {
        unsigned lv = rws(DGROUP, LEVEL_INDEX);
        amiga_tune(lv < sizeof g_level_song ? g_level_song[lv] : 4, 8, 0x20);
        break;
    }
    case 0x3739: amiga_tune(3, 0, 0x40); break;
    case 0x337B: amiga_tune(2, 0, 0x40); break;
    case 0x3389: amiga_tune(1, 0, 0x40); break;
    default: break;
    }
}

/* ---- the sound effects ---- */
#define GAME REL(0x013B)
#define SLOT(c, off) rws(DGROUP, (uint16_t)((c)->r[BX] + (off)))

static void fx(uint32_t note)
{
    if (g_log) fprintf(stderr, "snd: frame %lu effect %08lx\n", hal_frame_count(), (unsigned long)note);
    if (amiga_audio_ready()) tfmx_note(note);
}

/* the passengers' voices: the note by the passenger's kind (its descriptor) */
static uint32_t passenger_note(Cpu *c, int hail)
{
    switch (SLOT(c, 0x29AD)) {
    case 0x7720: case 0x776A: return hail ? 0x140B3328u : 0x0A083300u;
    case 0x77B4: case 0x77FE: return hail ? 0x0F0B3300u : 0x07083300u;
    case 0x7848: return hail ? 0x2A0B3380u : 0x14083300u;
    default: return hail ? 0x1E0B3380u : 0x14083300u;
    }
}

static void fx_hit(Cpu *c) { (void)c; fx(0x14003300u); }          /* a stone hits a creature or tree */
static void fx_ptero_cry(Cpu *c) { (void)c; fx(0x14013300u); }    /* the pterodactyl's warning cry */
static void fx_stone_lands(Cpu *c) { (void)c; fx(0x14023300u); }  /* the stone lands on a platform */
static void fx_dino_skid(Cpu *c) { (void)c; fx(0x14033300u); }    /* the dinosaur brakes */
static void fx_death(Cpu *c) { (void)c; fx(0x14043300u); }        /* crash, out of energy, hit */
static void fx_flap(Cpu *c) { if (SLOT(c, 0x2D2F) == 2) fx(0x14053300u); }    /* wing beat */
static void fx_step(Cpu *c) { if (SLOT(c, 0x2D2F) == 6) fx(0x14063300u); }    /* dinosaur step */
static void fx_roar(Cpu *c) { (void)c; fx(0x14073300u); }         /* the dinosaur notices the copter */
static void fx_complain(Cpu *c) { fx(passenger_note(c, 0)); }     /* left behind */
static void fx_pick_stone(Cpu *c) { (void)c; fx(0x14083300u); }
static void fx_drop_stone(Cpu *c) { (void)c; fx(0x14093300u); }
static void fx_hail(Cpu *c) { fx(passenger_note(c, 1)); }         /* a passenger hails the taxi */
static void fx_inhale(Cpu *c) { if (SLOT(c, 0x2D2F) == 0) fx(0x140C3300u); }  /* the blower sucks */
static void fx_blow(Cpu *c) { (void)c; fx(0x140D3300u); }         /* the blower blows */
static void fx_slot_done(Cpu *c) { (void)c; fx(0x140E2380u); }    /* a passenger stop is finished */
static void fx_key(Cpu *c, int space)                             /* typing (name, password) */
{
    uint8_t al = (uint8_t)c->r[AX];
    if (al == 8 || al == 13 || (space && al == ' ') || (al >= '0' && al <= '9') ||
        (al >= 'A' && al <= 'Z') || (al >= 'a' && al <= 'z'))
        fx(0x140A3300u);
}
static void fx_key_name(Cpu *c) { fx_key(c, 1); }
static void fx_key_password(Cpu *c) { fx_key(c, 0); }
static void fx_low_energy(Cpu *c)   /* once a frame: the Amiga beeps every 8 frames below 1/3 energy */
{
    static int count;
    int16_t e = (int16_t)rws(DGROUP, 0x2622);
    (void)c;
    if (e < 0 || e > 0x1E13) return;
    if (--count > 0) return;
    count += 8;
    if (count < 1) count = 8;
    fx(0x14031300u);
}

/* The cutscenes.  The Amiga scripts are commands (walk, jump, wait, play effect); the DOS ones
 * (engine at 024B:09D7) are per-actor lists of entries "sprite word, dx byte, dy byte", a
 * negative word opening a loop and 8000h closing it or ending the actor.  SI points into those
 * lists (DGROUP), so an entry is recognised by SI.  The matching moments:
 *   title (list DS:24DE, Amiga script 6F76) and ending (DS:1020, Amiga 7450): an actor jumps and
 *     lands, effect 02 on landing = the end of its list (2584, 13F4);
 *   intro (DS:1792 = Amiga list 6B44): a falling thing hits the ground and bounces, effect 00
 *     at the first bounce entry (1B0E, Amiga 6B9E); the caveman shakes his fist after his speech
 *     bubble, effect 08 when that loop starts (1A40, Amiga 6BF8).
 * The other intro (DS:1E40 = Amiga 6624) has no effects on the Amiga either. */
static void cut_end(Cpu *c)         /* 024B:0A45, an actor's list ended; SI = after its 8000h */
{
    /* (a finished actor rereads its 8000h every tick; [bx+2A4D] = FFFFh marks it finished) */
    if ((c->r[SI] == 0x2586 || c->r[SI] == 0x13F6) && SLOT(c, 0x2A4D) != 0xFFFF) fx(0x14023300u);
}
static void cut_entry(Cpu *c)       /* 024B:09FF, an entry read; SI = after its sprite word */
{
    if (c->r[SI] == 0x1B10) fx(0x14003300u);
}
static void cut_loop(Cpu *c)        /* 024B:0A23, a loop opened; SI = after its count word */
{
    if (c->r[SI] == 0x1A42) fx(0x14083300u);
}

static void on_song(Cpu *c) { log_call(c, "song"); play(arg(c, 6)); }
static void on_song2(Cpu *c) { log_call(c, "song2"); play(arg(c, 6)); }
static void on_stop(Cpu *c)
{
    log_call(c, "stop");
    g_tune = -1;
    if (amiga_audio_ready()) tfmx_fade(8, 0);
}
static void on_init(Cpu *c) { log_call(c, "init"); g_tune = -1; amiga_audio_stop(); }
static void on_done(Cpu *c) { log_call(c, "done"); g_tune = -1; amiga_audio_stop(); }

/* The game's "key hit?" test (024B:44F1, used by the intro, the title, the cutscenes and the other
 * "press a key" waits) reports any change of the last scancode its int 9 handler saw: presses,
 * releases, the E0/E1 prefixes and the modifiers alike.  So the host's own shortcuts (Ctrl+Shift+A
 * for the sound, Alt+Enter) skipped the intro through their Ctrl, Shift or Alt.  At 44FE the new
 * scancode is in AL and already stored in [2643]; a modifier or a prefix is turned back into the
 * previous scancode, so it is no key hit.  The controls in play read the key table instead (the
 * Ctrl fire buttons included), so they are not affected. */
static void key_hit_filter(Cpu *c)
{
    uint8_t raw = (uint8_t)c->r[AX], k = raw & 0x7F;
    if (raw == 0xE0 || raw == 0xE1 || k == 0x1D || k == 0x2A || k == 0x36 || k == 0x38) {
        uint8_t prev = rbs(DGROUP, 0x2645);
        wbs(DGROUP, 0x2643, prev);
        r8set(c, 0, prev);   /* AL */
    }
}

void game_hooks_after_load(void)
{
    g_log = getenv("UGH_SNDLOG") != NULL;
    gen_watch(REL(0x0878), 0x0F88, on_song);
    gen_watch(REL(0x0878), 0x1852, on_song2);
    gen_watch(REL(0x0878), 0x1059, on_stop);
    gen_watch(REL(0x0878), 0x0003, on_init);
    gen_watch(REL(0x0878), 0x008A, on_done);
    gen_watch(GAME, 0x252B, fx_hit);
    gen_watch(GAME, 0x28EE, fx_hit);
    gen_watch(GAME, 0x2A53, fx_hit);
    gen_watch(GAME, 0x2B0C, fx_hit);
    gen_watch(GAME, 0x23B0, fx_ptero_cry);
    gen_watch(GAME, 0x1D91, fx_stone_lands);
    gen_watch(GAME, 0x2830, fx_dino_skid);
    gen_watch(GAME, 0x40BA, fx_low_energy);
    gen_watch(GAME, 0x144C, fx_death);
    gen_watch(GAME, 0x40D5, fx_death);
    gen_watch(GAME, 0x2520, fx_death);
    gen_watch(GAME, 0x24E7, fx_flap);
    gen_watch(GAME, 0x277E, fx_step);
    gen_watch(GAME, 0x2667, fx_roar);
    gen_watch(GAME, 0x17E6, fx_complain);
    gen_watch(GAME, 0x1C48, fx_pick_stone);
    gen_watch(GAME, 0x1C81, fx_drop_stone);
    gen_watch(GAME, 0x0307, fx_key_name);
    gen_watch(GAME, 0x052C, fx_key_password);
    gen_watch(GAME, 0x16F6, fx_hail);
    gen_watch(GAME, 0x29C5, fx_inhale);
    gen_watch(GAME, 0x2995, fx_blow);
    gen_watch(GAME, 0x14AB, fx_slot_done);
    gen_watch(GAME, 0x0A45, cut_end);
    gen_watch(GAME, 0x09FF, cut_entry);
    gen_watch(GAME, 0x0A23, cut_loop);
    gen_watch(GAME, 0x44FE, key_hit_filter);
}
