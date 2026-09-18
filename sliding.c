// https://www-cs-faculty.stanford.edu/~knuth/programs/sliding.w
// https://en.wikipedia.org/wiki/15_puzzle

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include "gb_flip.h"
#include "sliding.h"

/* Tunables.  HASHBITS was 13 in the unpacked source, which makes the hash
   chains thousands deep once a level holds millions of configurations;
   22 costs 32MB and is about 10x faster on 4x4 puzzles.  MEMBITS sizes the
   configuration arena: it holds roughly 2^MEMBITS/4 configurations, i.e. the
   three deepest levels of the search must fit in that. */
#ifndef HASHBITS
#define HASHBITS 22
#endif
#ifndef MEMBITS
#define MEMBITS 25
#endif

constexpr int bdry = 999999;
constexpr int obst = 999998;
constexpr size_t maxsize = 256;
constexpr size_t boardsize = maxsize * 3 + 2;
constexpr size_t bufsize = 1024;
#define cell(j,k) board[ul+(j) *colsp+k]
constexpr size_t hashsize = (size_t)1 << HASHBITS;

constexpr size_t memsize = (size_t)1 << MEMBITS;
constexpr size_t maxmoves = 1000;

jmp_buf success_point;
int style;
int verbose;

/* --- non-local exit, replacing Knuth's exit() calls ------------------------ */
static jmp_buf fail_point;
static int fail_code;
static char fail_msg[256];

#define bail(code, ...) do { \
                snprintf(fail_msg, sizeof fail_msg, __VA_ARGS__); \
                fail_code = (code); \
                longjmp(fail_point, 1); \
        } while(0)

/* --- the puzzle specification is read from memory, not stdin -------------- */
static const char *specp;

static int spec_line(char *b)
{
        register char *q;
        register size_t n;
        if(!specp || !*specp)return 0;
        for(n = 0, q = b; *specp && *specp != '\n' && n < bufsize - 2; n++)*q++ = *specp++;
        while(*specp && *specp != '\n')specp++;
        if(*specp == '\n')specp++;
        *q++ = '\n', *q = '\0';
        return 1;
}

/* --- the solution, recorded level by level -------------------------------- */
static uint8_t path[(maxmoves + 1) * maxsize];
static int path_moves;
static int depth_limit;

int board[boardsize];
int aboard[boardsize];
int rows;
int cols;
int colsp;
int ul, lr;
int delta[4] = {1, -1};

int off[maxsize];
int offstart[16];
int curo;
char buf[bufsize];

int bcount;
int piece[maxsize], apiece[maxsize];
int place[maxsize], aplace[maxsize];

int balance[16];

char xboard[boardsize];
uint32_t config[maxsize / 8];

short uni[4][256];
uint32_t hash[hashsize];
uint32_t hashh[hashsize];

uint32_t pos[memsize + maxsize / 8 + 1];
uint32_t cutoff;
uint32_t cutoffh;
uint32_t curpos;
uint32_t curposh;
uint32_t source;
uint32_t sourceh;
uint32_t nextsource, nextsourceh;
uint32_t maxpos;
uint32_t maxposh;
uint32_t configs;
uint32_t configsh;
uint32_t oldconfigs;
uint32_t milestone[maxmoves];
uint32_t milestoneh[maxmoves];
uint32_t shortcut;
int goalhash;
uint32_t goal[maxsize / 8];
uint32_t start[maxsize / 8];

int head[maxsize + 1], out[maxsize + 1], in[maxsize + 1];
int link[boardsize], olink[boardsize], ilink[boardsize];

int perm[maxsize + 1], iperm[maxsize + 1];
char decision[maxsize];
int inx[maxsize], lstart[maxsize];
int super[maxsize];

static inline void boardover(void)
{
        bail(SL_ERR_BOARD, "board too large; recompile with a larger maxsize");
}

static int hashcode(unsigned int x)
{
        return uni[0][x & 0xff] + uni[1][(x >> 8) & 0xff] + uni[2][(x >> 16) & 0xff] + uni[3][x >> 24];
}

int fill_board(int board[], int piece[], int place[])
{
        register int j, c, k, t;
        register char*p;
        for(j = 0; j < ul; j++)board[j] = bdry;
        for(j = ul; j <= lr; j++)board[j] = -1;
        for(j = ul + cols; j <= lr; j += colsp)board[j] = bdry;
        for(; j <= lr + colsp; j++)board[j] = bdry;
        for(p = &buf[0], j = ul, bcount = c = 0; *p != '\n'; p++) {
                while(board[j] >= 0)if(++j > lr)return -1;
                if(*p == '0')board[j] = t = 0;
                else if(*p >= '1' && *p <= '9')t = *p - '0';
                else if(*p >= 'a' && *p <= 'f')t = *p - ('a' - 10);
                else if(*p == 'x')t = 0, board[j] = obst;
                else return -2;
                if(t) {
                        bcount++;
                        piece[bcount] = t;
                        place[bcount] = j;
                        board[j] = bcount;
                        for(k = offstart[t]; off[k]; k++)
                                if(j + off[k] < ul || j + off[k] > lr || board[j + off[k]] >= 0)c++;
                                else board[j + off[k]] = bcount;
                }
                j++;
        }
        for(; j <= lr; j++)if(board[j] < 0)board[j] = 0;
        return c;
}

/* Snapshot one level of the solution as rows*cols bytes of piece types. */
static void record(int level, int b[], int pc[])
{
        register int j, k, c;
        register uint8_t *q = &path[level * rows * cols];
        for(j = 0; j < rows; j++)
                for(k = 0; k < cols; k++) {
                        c = b[ul + j * colsp + k];
                        *q++ = c >= obst ? SL_OBSTACLE : (uint8_t)(c ? pc[c] : 0);
                }
}

void print_board(int board[], int piece[])
{
        register int j, k;
        for(j = 0; j < rows; j++) {
                for(k = 0; k < cols; k++)
                        printf(" %c",
                               cell(j, k) == cell(j - 1, k) && cell(j, k) && cell(j, k) < obst ? '|' : ' ');
                printf("\n");
                for(k = 0; k < cols; k++)
                        if(cell(j, k) < 0)printf(" ?");
                        else if(cell(j, k) < obst)printf("%c%x",
                                                                 cell(j, k) == cell(j, k - 1) && cell(j, k) && cell(j, k) < obst ? '-' : ' ',
                                                                 piece[cell(j, k)]);
                        else printf("  ");
                printf("\n");
        }
}

int pack(int board[], int piece[])
{
        register int i, j, k, p, s, t;
        for(j = ul; j <= lr; j++)xboard[j] = 0;
        for(i = s = 0, p = 28, j = ul, t = bcount; t; j++)if(board[j] < obst && !xboard[j]) {
                        k = piece[board[j]];
                        if(k) {
                                t--, s += k << p;
                                for(k = offstart[k]; off[k]; k++)xboard[j + off[k]] = 1;
                        }
                        if(!p)config[i++] = s, s = 0, p = 28;
                        else p -= 4;
                }
        if(p != 28)config[i++] = s;
        return i;
}

void print_config(uint32_t config[], int n)
{
        register int j, t;
        for(j = 0; j < n - 1; j++)printf("%08x", config[j]);
        for(t = config[n - 1], j = 8; (t & 0xf) == 0; j--)t >>= 4;
        printf("%0*x", j, t);
}

int unpack(int board[], int piece[], int place[], uint32_t config[])
{
        register int i, j, k, p, s, t;
        for(j = ul; j <= lr; j++)xboard[j] = 0;
        for(p = i = 0, j = ul, t = bcount; t; j++)
                if(board[j] < obst && !xboard[j]) {
                        if(!p)s = config[i++], p = 28;
                        else p -= 4;
                        k = (s >> p) & 0xf;
                        if(k) {
                                board[j] = t, piece[t] = k, place[t] = j;
                                for(k = offstart[k]; off[k]; k++)xboard[j + off[k]] = 1, board[j + off[k]] = t;
                                t--;
                        } else board[j] = 0;
                }
        for(; j <= lr; j++)if(board[j] < obst && !xboard[j])board[j] = 0;
        return i;
}

void print_big(uint32_t hi, uint32_t lo)
{
        printf("%.15g", ((double)hi) * 4294967296.0 + (double)lo);
}

void print_bigx(uint32_t hi, uint32_t lo)
{
        if(hi)printf("%x%08x", hi, lo);
        else printf("%x", lo);
}

int hashin(int trick)
{
        register int h, j, k, n, bound;
        n = pack(board, piece);
        for(h = hashcode(config[0]), j = 1; j < n; j++)h ^= hashcode(config[j]);
        h &= hashsize - 1;
        if(hashh[h] == cutoffh) {
                if(hash[h] < cutoff)goto newguy;
        } else if(hashh[h] < cutoffh)goto newguy;
        bound = hash[h] - cutoff;
        for(j = hash[h] & (memsize - 1);; j = (j - pos[j]) & (memsize - 1)) {
                for(k = 0; k < n; k++)if(config[k] != pos[j + 2 + k])goto nope;
                if(trick) {
                        if(bound < shortcut)return 0;
                        n = (j - source) & (memsize - 1);
                        if(pos[j + 1] == n)return 0;
                        pos[j + 1] = n;
                        return 1;
                }

                return 0;
nope:
                bound -= pos[j];
                if(bound < 0)break;
        }
newguy:
        j = curpos & (memsize - 1);
        pos[j] = curpos - hash[h];
        if(pos[j] > memsize || curposh > hashh[h] + (pos[j] > curpos))
                pos[j] = memsize;
        pos[j + 1] = curpos - source;
        for(k = 0; k < n; k++)pos[j + 2 + k] = config[k];
        hash[h] = curpos, hashh[h] = curposh;

        if(verbose >= 0 && (configs == oldconfigs || verbose > 0)) {
                print_config(config, n);
                if(verbose > 0) {
                        printf(" (");
                        print_big(configsh, configs);
                        printf("=#");
                        print_bigx(curposh, curpos);
                        printf(", from #");
                        print_bigx(sourceh, source);
                        printf(")\n");
                }
        }
        configs++;
        if(configs == 0)configsh++;

        curpos += n + 2;
        if(curpos < n + 2)curposh++;
        if((curpos & (memsize - 1)) < n + 2)curpos &= -memsize;
        if(curposh == maxposh) {
                if(curpos <= maxpos)goto okay;
        } else if(curposh < maxposh)goto okay;
        bail(SL_ERR_MEM, "search outgrew memsize (2^%d words); "
             "the puzzle is too deep for this build", MEMBITS);
okay:
        if(h == goalhash) {
                for(k = 0; k < n; k++)if(config[k] != goal[k])goto not_yet;
                longjmp(success_point, 1);
not_yet:
                ;
        }

        return trick;
}

void move(int k, int del, int delo)
{
        register int j, s, t;
        s = place[k], t = piece[k];
        for(j = offstart[t];; j++) {
                board[s + off[j]] = 0;
                if(!off[j])break;
        }
        for(j = offstart[t];; j++) {
                if(board[s + del + off[j]])goto illegal;
                if(!off[j])break;
        }
        for(j = offstart[t];; j++) {
                board[s + del + off[j]] = k;
                if(!off[j])break;
        }
        if(hashin(style == 2) || style == 1)

        {
                for(j = offstart[t];; j++) {
                        board[s + del + off[j]] = 0;
                        if(!off[j])break;
                }
                for(j = offstart[t];; j++) {
                        board[s + off[j]] = k;
                        if(!off[j])break;
                }
                if(style == 1)move(k, del + delo, delo);
                else for(j = 0; j < 4; j++)if(delta[j] != -delo)move(k, del + delta[j], delta[j]);
        } else {
                for(j = offstart[t];; j++) {
                        board[s + del + off[j]] = 0;
                        if(!off[j])break;
                }
illegal:
                for(j = offstart[t];; j++) {
                        board[s + off[j]] = k;
                        if(!off[j])break;
                }
        }
}

void supermove(int, int);
void ideals(int del)
{
        register int j, k, l, p, u, v, t;
        for(j = 0; j <= bcount; j++)perm[j] = iperm[j] = j;
        l = p = 0;
excl:
        decision[l] = 0, lstart[l] = p;
        for(j = inx[l], t = j + 1; j < t; j++) {
                v = perm[j];
                for(k = in[v]; k >= 0; k = ilink[k]) {
                        u = aboard[k];
                        if(iperm[u] >= t) {
                                register int uu = perm[t], tt = iperm[u];
                                perm[t] = u, perm[tt] = uu, iperm[u] = t, iperm[uu] = tt;
                                t++;
                        }
                }
                if(decision[0] == 1)
                        for(v = head[v]; v >= 0; v = link[v])super[p++] = v;
        }

        if(t > bcount) {
                if(p) {
                        super[p] = 0;
                        if(decision[0] == 0)supermove(del, del);
                        else supermove(-del, -del);
                }
                goto incl;
        }
        inx[++l] = t;
        goto excl;
incl:
        decision[l] = 1, p = lstart[l];
        for(j = inx[l], t = j + 1; j < t; j++) {
                u = perm[j];
                for(k = out[u]; k >= 0; k = olink[k]) {
                        v = aboard[k];
                        if(iperm[v] >= t) {
                                register int vv = perm[t], tt = iperm[v];
                                perm[t] = v, perm[tt] = vv, iperm[v] = t, iperm[vv] = tt;
                                t++;
                        }
                }
                if(decision[0] == 0)
                        for(u = head[u]; u >= 0; u = link[u])super[p++] = u;
        }

        if(t > bcount) {
                if(p) {
                        super[p] = 0;
                        if(decision[0] == 0)supermove(del, del);
                        else supermove(-del, -del);
                }

                goto backup;
        }
        inx[++l] = t;
        goto excl;
backup:
        if(l) {
                l--;
                if(decision[l])goto backup;
                goto incl;
        }
}

void supermove(int del, int delo)
{
        register int j;
        for(j = 0; super[j]; j++) {
                board[super[j]] = 0;
        }
        for(j = 0; super[j]; j++) {
                if(board[del + super[j]])goto illegal;
        }
        for(j = 0; super[j]; j++) {
                board[del + super[j]] = aboard[super[j]];
        }
        if(hashin(style == 5) || style == 4) {
                for(j = 0; super[j]; j++) {
                        board[del + super[j]] = 0;
                }
                for(j = 0; super[j]; j++) {
                        board[super[j]] = aboard[super[j]];
                }
                if(style == 4)supermove(del + delo, delo);
                else for(j = 0; j < 4; j++)
                                if(delta[j] != -delo)supermove(del + delta[j], delta[j]);
        } else {
                for(j = 0; super[j]; j++) {
                        board[del + super[j]] = 0;
                }
illegal:
                for(j = 0; super[j]; j++) {
                        board[super[j]] = aboard[super[j]];
                }
        }
}

static void run(void)
{
        register int j, t, k;
        volatile int d;

        if(style < 0 || style > 5)
                bail(SL_ERR_STYLE, "style should be between 0 and 5, not %d", style);

        if(!spec_line(buf) ||
                        sscanf(buf, "%d x %d", &rows, &cols) != 2 || rows <= 0 || cols <= 0)
                bail(SL_ERR_SPEC, "bad specification of rows x cols");
        if(rows * cols > maxsize)boardover();
        colsp = cols + 1;
        delta[2] = colsp, delta[3] = -colsp;
        ul = colsp;
        lr = (rows + 1) * colsp - 2;

        for(j = 1; j < 16; j++)offstart[j] = -1;
        while(1) {
                if(!spec_line(buf)) {
                        buf[0] = '\n';
                        break;
                }
                if(buf[0] == '\n')continue;
                if(buf[1] != ' ' || buf[2] != '=' || buf[3] != ' ')break;
                if(buf[0] >= '1' && buf[0] <= '9')t = buf[0] - '0';
                else if(buf[0] >= 'a' && buf[0] <= 'f')t = buf[0] - ('a' - 10);
                else bail(SL_ERR_SPEC, "bad piece name (%c)", buf[0]);
                if(offstart[t] >= 0)
                        printf("Warning: Redefinition of piece %c is being ignored.\n", buf[0]);
                else {
                        offstart[t] = curo;
                        {
                                register char*p;
                                for(t = -1, j = k = 0, p = &buf[4];; p++)
                                        switch(*p) {
                                        case'1':if(t<0)t= k;else off[curo++]= k-t;
                                                        if(curo >= maxsize)boardover();
                                                        case'0':j++,k++;break;
                                        case'/':
                                                k += colsp - j, j = 0;
                                                break;
                                        case'\n':
                                                goto offsets_done;
                                        default:
                                                bail(SL_ERR_SPEC,
                                                     "bad character `%c' in definition of piece %c", *p, buf[0]);
                                        }
offsets_done:
                                if(t < 0)bail(SL_ERR_SPEC, "piece %c is empty", buf[0]);
                                off[curo++] = 0;
                                if(curo >= maxsize)boardover();
                        }

                }
        }

        t = fill_board(board, piece, place);
        if(verbose >= 0) {
                printf("Starting configuration:\n");
                print_board(board, piece);
        }
        if(t)bail(SL_ERR_SPEC, "starting configuration: %s",
                          t == 1 ? "a cell was filled twice" :
                          t > 0 ? "cells were filled twice" :
                          t == -1 ? "the board wasn't big enough" :
                          "illegal character in the configuration");
        if(bcount == 0)bail(SL_ERR_SPEC, "the puzzle doesn't have any pieces");
        if(!spec_line(buf))bail(SL_ERR_SPEC, "missing stopping configuration");
        t = fill_board(aboard, apiece, aplace);
        if(verbose >= 0) {
                printf("\nStopping configuration:\n");
                print_board(aboard, apiece);
        }
        if(t)bail(SL_ERR_SPEC, "stopping configuration: %s",
                          t == 1 ? "a cell was filled twice" :
                          t > 0 ? "cells were filled twice" :
                          t == -1 ? "the board wasn't big enough" :
                          "illegal character in the configuration");
        for(j = 0; j < 16; j++)balance[j] = 0;
        for(j = ul; j <= lr; j++) {
                if((board[j] < obst) != (aboard[j] < obst))
                        bail(SL_ERR_SPEC, "the dead cells (x's) are in different places");
                if(board[j] < obst)
                        balance[piece[board[j]]]++, balance[apiece[aboard[j]]]--;
        }
        for(j = 0; j < 16; j++)if(balance[j])
                        bail(SL_ERR_SPEC, "wrong number of pieces in the stopping configuration");

        gb_init_rand(0);
        for(j = 0; j < 4; j++)for(k = 1; k < 256; k++)uni[j][k] = gb_next_rand();

        if(setjmp(success_point))goto hurray;

        if(verbose >= 0)printf("\n(using moves of style %d)\n", style);

        record(0, board, piece);
        t = pack(board, piece);
        for(k = 0; k < t; k++)start[k] = config[k];

restart:
        t = pack(aboard, apiece);
        for(k = goalhash = 0; k < t; k++)goal[k] = config[k], goalhash ^= hashcode(config[k]);
        goalhash &= hashsize - 1;

        curpos = cutoff = milestone[0] = 1, curposh = cutoffh = milestoneh[0] = 0;
        source = sourceh = configs = configsh = oldconfigs = d = 0;
        maxposh = 1;
        if(verbose >= 0)printf("*** Distance 0:\n");
        hashin(0);
        if(verbose == 0)printf(".\n");

        for(d = 1; d < maxmoves; d++) {
                if(depth_limit > 0 && d > depth_limit)
                        bail(SL_ERR_DEPTH, "no solution within %d moves", depth_limit);
                if(verbose >= 0)printf("*** Distance %d:\n", d);
                milestone[d] = curpos, milestoneh[d] = curposh;
                oldconfigs = configs;
                if(d > 1)cutoff = milestone[d - 2], cutoffh = milestoneh[d - 2];
                shortcut = curpos - cutoff;
                maxpos = cutoff + memsize, maxposh = cutoffh + (maxpos < memsize);
                for(source = milestone[d - 1], sourceh = milestoneh[d - 1];
                                source != milestone[d] || sourceh != milestoneh[d];
                                source = nextsource, sourceh = nextsourceh) {
                        j = unpack(board, piece, place, &pos[(source & (memsize - 1)) + 2]) + 2;
                        nextsource = source + j, nextsourceh = sourceh + (nextsource < j);
                        if((nextsource & (memsize - 1)) < j)nextsource &= -memsize;
                        if(style < 3)
                                for(j = 0; j < 4; j++)
                                        for(k = 1; k <= bcount; k++)move(k, delta[j], delta[j]);
                        else {
                                for(j = 0; j <= bcount; j++)head[j] = -1;
                                for(j = 0; j <= lr + colsp; j++) {
                                        k = board[j];
                                        if(k) {
                                                if(k >= obst)k = 0;
                                                aboard[j] = k;
                                                link[j] = head[k];
                                                head[k] = j;
                                        } else aboard[j] = -1;
                                }

                                for(j = 0; j <= bcount; j++)out[j] = in[j] = -1;
                                for(j = 0; j <= bcount; j++)
                                        for(k = head[j]; k >= ul; k = link[k]) {
                                                t = aboard[k - colsp];
                                                if(t != j && t >= 0 && (out[t] < 0 || aboard[out[t]] != j)) {
                                                        olink[k] = out[t], out[t] = k;
                                                        ilink[k - colsp] = in[j], in[j] = k - colsp;
                                                }
                                        }

                                ideals(colsp);
                                head[0] = lr + 1;

                                for(j = 0; j <= bcount; j++)out[j] = in[j] = -1;
                                for(j = 0; j <= bcount; j++)
                                        for(k = head[j]; k >= ul; k = link[k]) {
                                                t = aboard[k - 1];
                                                if(t != j && t >= 0 && (out[t] < 0 || aboard[out[t]] != j)) {
                                                        olink[k] = out[t], out[t] = k;
                                                        ilink[k - 1] = in[j], in[j] = k - 1;
                                                }
                                        }
                                ideals(1);
                        }
                }

                if(configs == oldconfigs)
                        bail(SL_ERR_UNSOLVED, "no solution: the reachable state space "
                             "was exhausted at distance %d", d);
                if(verbose == 0)printf(" and %d more.\n", configs - oldconfigs - 1);
        }
        bail(SL_ERR_DEPTH, "no solution within maxmoves=%zu", maxmoves);

hurray:
        if(verbose >= 0)printf("... Solution!\n");
        if(path_moves < 0)path_moves = d;
        if(d == 0)fail_code = SL_OK, longjmp(fail_point, 1);
        record(d, board, piece);

        if(curposh || curpos > memsize) {
                maxpos = curpos - memsize;
                maxposh = curposh - (maxpos > curpos);
        } else maxpos = maxposh = 0;
        for(j = 0; j <= lr + colsp; j++)aboard[j] = board[j];
        while(sourceh > maxposh || (sourceh == maxposh && source >= maxpos)) {
                d--;
                if(d == 0)fail_code = SL_OK, longjmp(fail_point, 1);
                k = source & (memsize - 1);
                unpack(aboard, apiece, aplace, &pos[k + 2]);
                record(d, aboard, apiece);
                if(verbose >= 0) {
                        printf("\n%d:\n", d);
                        print_board(aboard, apiece);
                }
                if(source < pos[k + 1])sourceh--;
                source = source - pos[k + 1];
        }

        if(verbose >= 0) {
                printf("(Unfortunately I've forgotten how to get to level %d,\n", d);
                printf(" so I'll have to reconstruct that part. Please bear with me.)\n");
        }
        for(j = 0; j < hashsize; j++)hash[j] = hashh[j] = 0;
        unpack(board, piece, place, start);
        goto restart;
}

/* ------------------------------------------------------------------------ */
/* Public interface                                                          */
/* ------------------------------------------------------------------------ */

int sliding_solve(const char *spec, int style_, int verbose_, int maxdepth)
{
        register size_t j;

        style = style_, verbose = verbose_;
        depth_limit = maxdepth;
        specp = spec;
        path_moves = -1;
        fail_code = SL_OK;
        fail_msg[0] = '\0';
        curo = bcount = 0;
        rows = cols = 0;
        for(j = 0; j < hashsize; j++)hash[j] = hashh[j] = 0;

        if(!setjmp(fail_point))run();
        if(fail_code != SL_OK)return fail_code;
        if(path_moves < 0)path_moves = 0;      /* already solved */
        return path_moves;
}

int sliding_moves(void)
{
        return path_moves < 0 ? 0 : path_moves;
}

int sliding_rows(void)
{
        return rows;
}

int sliding_cols(void)
{
        return cols;
}

const uint8_t *sliding_path(void)
{
        return path;
}

const char *sliding_message(void)
{
        return fail_msg;
}

#ifndef SLIDING_NO_MAIN
int main(int argc, char *argv[])
{
        int st, vb = 0, rc;
        size_t n = 0, cap = 1 << 16;
        char *spec;

        if(!(argc >= 2 && sscanf(argv[1], "%d", &st) == 1 &&
                        (argc == 2 || sscanf(argv[2], "%d", &vb) == 1))) {
                fprintf(stderr, "Usage: %s style [verbose]\n", argv[0]);
                return 1;
        }

        spec = malloc(cap);
        if(!spec)return 1;
        for(;;) {
                size_t got = fread(spec + n, 1, cap - n - 1, stdin);
                n += got;
                if(n + 1 < cap)break;
                cap *= 2;
                spec = realloc(spec, cap);
                if(!spec)return 1;
        }
        spec[n] = '\0';

        rc = sliding_solve(spec, st, vb, 0);
        free(spec);
        if(rc < 0) {
                fprintf(stderr, "%s!\n", sliding_message());
                return -rc;
        }
        printf("\nSolved in %d moves.\n", rc);
        return 0;
}
#endif
