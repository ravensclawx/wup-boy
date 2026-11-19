/* WUP Boy: the Game Boy and Game Boy Color games in this app's roms/ folder (.gb, .gbc: copied there with File Sharing), in the Homebrew Menu's
 * look (libwup's kit). A game starts the player (player.wup, next to this app: player/main.c) with the game's path;
 * the player comes back here when the game is left. No popups: an empty folder says what to do. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "wup_kit.h"
#include "littlefs/lfs.h"

WUP_APP("WUP Boy");

#define MAXG 48
static struct { char file[64], name[64]; int color; } games[MAXG];
static int ngames, started;
static char roms[80];

static int is_rom(const char *n) { size_t l = strlen(n); return (l > 3 && !strcasecmp(n + l - 3, ".gb")) || (l > 4 && !strcasecmp(n + l - 4, ".gbc")); }
static int by_name(const void *a, const void *b) { return strcasecmp(((const char *)a) + 64, ((const char *)b) + 64); }
static void scan(void) {
    ngames = 0;
    snprintf(roms, sizeof roms, "%s/roms", wup_app_dir()[0] ? wup_app_dir() : "/apps/wup_boy");
    if (!wup_fs_ok()) return;
    lfs_mkdir(&fs_lfs, roms);                              /* (there for whoever looks over File Sharing) */
    lfs_dir_t d; struct lfs_info inf;
    if (lfs_dir_open(&fs_lfs, &d, roms)) return;
    while (lfs_dir_read(&fs_lfs, &d, &inf) > 0 && ngames < MAXG) {
        if (inf.type != LFS_TYPE_REG || !is_rom(inf.name) || strlen(inf.name) >= sizeof games[0].file) continue;
        snprintf(games[ngames].file, sizeof games[0].file, "%s", inf.name);
        snprintf(games[ngames].name, sizeof games[0].name, "%s", inf.name); *strrchr(games[ngames].name, '.') = 0;
        char path[160]; uint8_t cgb = 0; snprintf(path, sizeof path, "%s/%s", roms, inf.name);
        games[ngames].color = fs_read(path, 0x143, &cgb, 1) == 1 && (cgb & 0x80);     /* the cartridge header's colour flag */
        ngames++;
    }
    lfs_dir_close(&fs_lfs, &d);
    qsort(games, (size_t)ngames, sizeof games[0], by_name);
}
static void on_game(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char player[96], rom[160];
    snprintf(player, sizeof player, "%s/player.wup", wup_app_dir()[0] ? wup_app_dir() : "/apps/wup_boy");
    snprintf(rom, sizeof rom, "%s/%s", roms, games[i].file);
    if (!wup_exec(player, rom)) started = 1;               /* (this app ends; the player is next) */
}
static void home(lv_obj_t *scr) {
    if (!wup_fs_ok()) { kit_outcome(kit_page(scr, "WUP Boy", "B Back"), "Storage Not Available", ""); return; }
    if (!ngames) {
        kit_empty(kit_page(scr, "WUP Boy", "B Back"), "No Games", "Add .gb and .gbc files to apps/wup_boy/roms with File Sharing");
        return;
    }
    lv_obj_t *l = kit_page(scr, "WUP Boy", "B Back    A Play");
    for (int i = 0; i < ngames; i++) kit_row(l, NULL, games[i].name, games[i].color ? "Color" : NULL, on_game, (void *)(intptr_t)i);
}
int main(void) {
    scan();
    kit_start(home);
    while (!started && kit_step()) {}
    return 0;
}
