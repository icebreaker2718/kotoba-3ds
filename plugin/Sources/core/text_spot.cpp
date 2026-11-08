#include "core/text_spot.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "core/team.hpp"
#include "vocr_textspot6.h"

namespace kt {

TextSpotter::~TextSpotter() {
    unload();
    free(scratch_);
}

void TextSpotter::unload() {
    free(model_);
    free(blob_);
    model_ = blob_ = nullptr;
}

namespace {
// The spotter's call for a team (core/team.hpp): member 0 runs it, publishing each row's blocks as a job.
struct SpotTeam : Team {
    const vocr_textspot6 *m = nullptr;
    const VocrLinesImage *im = nullptr;
    void *arena = nullptr;
    size_t need = 0;
    VocrTextspot6Set *set = nullptr;
    vocr_textspot6_par par;
    int rc = VOCR_LINES_E_ARG;
};

void teamMember(void *arg) {
    SpotTeam *t = (SpotTeam *)arg;
    const unsigned w = t->join();
    if (w == 0) {
        t->rc = vocr_textspot6_find_par(t->m, t->im, 0, t->arena, t->need, t->set, &t->par);
        t->finish();
        return;
    }
    t->help(w);
}
}  // namespace

bool TextSpotter::load(Reader &file, std::string &error) {
    if (model_) return true;
    const uint32_t n = file.size();
    if (n < 64 || n > 1024 * 1024) { error = "the text spotter file looks wrong"; return false; }
    blob_ = malloc(n + 32);  // + the bytes the ARM11 kernel may read past the end
    vocr_textspot6 *m = (vocr_textspot6 *)malloc(sizeof(vocr_textspot6));
    if (!blob_ || !m) {
        free(m);
        unload();
        error = "not enough memory for the text spotter";
        return false;
    }
    if (!file.read(0, blob_, n)) {
        free(m);
        unload();
        error = "could not read the text spotter";
        return false;
    }
    if (vocr_textspot6_load(m, blob_, n) != VOCR_LINES_OK || m->version != VOCR_TEXTSPOT6_VERSION_TAG) {
        free(m);
        unload();
        error = "the text spotter file is damaged or not a spotter";
        return false;
    }
    vocr_textspot6_pack_simd32(m);  // ARM11: the 1x1 layers' weights reordered for its SIMD kernel
    model_ = m;
    return true;
}

size_t TextSpotter::arenaBytes(unsigned w, unsigned h) const {
    return model_ ? vocr_textspot6_frame_arena_bytes((const vocr_textspot6 *)model_, w, h) : 0;
}

bool TextSpotter::find(const uint8_t *grey, unsigned w, unsigned h, std::vector<SpotBox> &boxes,
                       std::string &error) const {
    boxes.clear();
    if (!model_) { error = "no text spotter loaded"; return false; }
    const vocr_textspot6 *m = (const vocr_textspot6 *)model_;
    const size_t need = vocr_textspot6_frame_arena_bytes(m, w, h);
    void *arena = need ? malloc(need) : nullptr;
    VocrTextspot6Set *set = (VocrTextspot6Set *)malloc(sizeof(VocrTextspot6Set));
    if (!arena || !set) {
        const bool arenaOk = arena != nullptr, setOk = set != nullptr;
        free(arena);
        free(set);
        if (memFail_) memFail_(need, arenaOk, setOk);
        error = "not enough memory to look for text";
        return false;
    }
    VocrLinesImage im;
    im.pixels = grey;
    im.width = w;
    im.height = h;
    im.stride = w;
    im.bpp = VOCR_LINES_GREY;
    unsigned workers = runner_ ? std::min<unsigned>(workers_, VOCR_TEXTSPOT6_PAR_MAX) : 1u;
    if (workers > 1 && scratchFor_ < workers) {  // kept for the session: 1.5 KB a worker
        free(scratch_);
        scratch_ = (signed char *)malloc((size_t)(workers - 1) * VOCR_TEXTSPOT6_PAR_TMP);
        scratchFor_ = scratch_ ? workers : 0;
    }
    if (!scratch_) workers = 1;
    int r;
    if (workers > 1) {
        SpotTeam team;
        team.m = m;
        team.im = &im;
        team.arena = arena;
        team.need = need;
        team.set = set;
        memset(&team.par, 0, sizeof team.par);
        team.workers = workers;
        team.par.for_each = Team::forEachC;
        team.par.team = (Team *)&team;
        team.par.workers = workers;
        for (unsigned k = 1; k < workers; k++) team.par.tmp[k] = scratch_ + (size_t)(k - 1) * VOCR_TEXTSPOT6_PAR_TMP;
        runner_(workers, teamMember, &team);
        r = team.rc;
    } else {
        r = vocr_textspot6_find(m, &im, 0, arena, need, set);
    }
    if (r == VOCR_LINES_OK) {
        for (unsigned i = 0; i < set->count; i++) {
            const VocrTextspot6Box &b = set->boxes[i];
            SpotBox s;
            s.x = b.x;
            s.y = b.y;
            s.w = b.w;
            s.h = b.h;
            s.tag = b.tag;
            s.conf = b.conf;
            s.block = b.block;
            s.dark = b.dark != 0;
            s.vertical = b.vertical != 0;
            boxes.push_back(s);
        }
    } else {
        error = "the text spotter failed";
    }
    free(arena);
    free(set);
    return r == VOCR_LINES_OK;
}

}  // namespace kt
