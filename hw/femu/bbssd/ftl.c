#include "ftl.h"

// #define FEMU_DEBUG_FTL

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa);

struct p2l_entry *p2l_table = NULL;
struct hash_ppa_entry *hash_ppa_table = NULL;

void p2l_push(struct ssd *ssd, struct ppa *ppa, uint64_t lpn) {
    uint64_t ppn = ppa2pgidx(ssd, ppa);
    struct p2l_entry *entry;
    HASH_FIND(hh, p2l_table, &ppn, sizeof(uint64_t), entry);  // 기존 엔트리 존재 여부 확인
    if (entry == NULL) {
        entry = (struct p2l_entry *)malloc(sizeof(struct p2l_entry));  // 새 엔트리 생성
        entry->ppn = ppn;
        HASH_ADD(hh, p2l_table, ppn, sizeof(uint64_t), entry);  // 테이블에 추가
    }
    entry->lpn = lpn;  // LPN 설정
}

uint64_t p2l_find(struct ssd *ssd, struct ppa *ppa) {
    uint64_t ppn = ppa2pgidx(ssd, ppa);
    struct p2l_entry *entry;
    HASH_FIND(hh, p2l_table, &ppn, sizeof(uint64_t), entry);  // 엔트리 찾기
    if (entry) {
        return entry->ppn;  // PPN 반환
    } else {
        return -1;  // 엔트리 없음
    }
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa) {
    // 채널에서 ppa에 해당하는 채널을 반환
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa) {
    // 채널->룬에서 ppa에 해당하는 룬을 반환
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa) {
    // 채널->룬->플레인에서 ppa에 해당하는 플레인을 반환
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa) {
    // 채널->룬->플레인->블록에서 ppa에 해당하는 블록을 반환
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa) {
    // 현재 블록ID에 해당하는 라인을 반환
    // TODO: 슈퍼블록으로 바꿀거니 인풋에 룬도 주면 될 듯
    // lines[ppa->g.blk][get_lun()] 이런식?
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa) {
    // 채널->룬->플레인->블록->페이지에서 ppa에 해당하는 페이지를 반환
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static int get_block_ref_cnt(struct ssd *ssd, struct ppa *ppa) {
    struct nand_block *blk = get_blk(ssd, ppa);
    return blk->rpc;
}

static bool could_get_hash_support(unsigned char *hash, unsigned int len) {
    struct hash_ppa_entry *entry;
    HASH_FIND(hh, hash_ppa_table, hash, len, entry);
    if (entry == NULL) return false;

    bool there_is_space = false;
    struct ppa_entry *ppa_item = NULL, *tmp = NULL;
    HASH_ITER(hh, entry->ppa_table, ppa_item, tmp) {
        if (ppa_item->cnt < 15) {
            there_is_space = true;
            break;
        }
    }

    return there_is_space;
}

static void print_blk(struct ssd *ssd) {
    static int cnt = 0;
    cnt++;
    if (cnt % 100000 != 0) return;
    for (int i = 0; i < ssd->sp.tt_blks; i++) {
        struct nand_block *blk = get_blk(ssd, &((struct ppa){.ppa = i}));
        printf("(%d,%d) ", i, blk->rpc);
    }
    printf("\n");
}

static void add_one_in_hash(struct ssd *ssd, unsigned char *hash, unsigned int len) {
    struct hash_ppa_entry *entry;
    HASH_FIND(hh, hash_ppa_table, hash, len, entry);

    struct ppa_entry *ppa_item = NULL, *tmp = NULL;
    HASH_ITER(hh, entry->ppa_table, ppa_item, tmp) {
        if (ppa_item->cnt < 15) {
            struct ppa ppa;
            ppa.ppa = ppa_item->uintppa;

            struct nand_page *pg = get_pg(ssd, &ppa);
            struct nand_block *blk = get_blk(ssd, &ppa);
            struct line *line = get_line(ssd, &ppa);

            pg->rpc++;
            blk->rpc++;
            line->rpc++;

            ppa_item->cnt++;

            // printf("MYPRINT| HIT!: added one {%ld, %d}\n", ppa_item->uintppa, ppa_item->cnt);
            return;
        }
    }
}

static void map_sha256_to_ppa(unsigned char *hash, unsigned int len, struct ppa *ppa) {
    struct hash_ppa_entry *entry;
    HASH_FIND(hh, hash_ppa_table, hash, len, entry);

    if (entry == NULL) {
        // 중복이 없으면 새로운 엔트리 추가
        entry = (struct hash_ppa_entry *)malloc(sizeof(struct hash_ppa_entry));
        memcpy(entry->hash, hash, len);  // 해시 값을 구조체에 복사

        entry->ppa_table = NULL;                         // 내부 맵(ppa 테이블) 초기화
        HASH_ADD(hh, hash_ppa_table, hash, len, entry);  // 해시 테이블에 추가

        struct ppa_entry *ppa_item = (struct ppa_entry *)malloc(sizeof(struct ppa_entry));
        uint64_t uintppa = ppa->ppa;
        ppa_item->uintppa = uintppa;
        ppa_item->cnt = 0;
        HASH_ADD(hh, entry->ppa_table, uintppa, sizeof(uint64_t), ppa_item);  // 내부 맵에 추가
        // printf("MYPRINT| is new entry {%ld, %d} - %ld\n", uintppa, ppa_item->cnt, ppa->ppa);
        return;
    }

    struct ppa_entry *ppa_item = (struct ppa_entry *)malloc(sizeof(struct ppa_entry));
    uint64_t uintppa = ppa->ppa;
    ppa_item->uintppa = uintppa;
    ppa_item->cnt = 0;
    HASH_ADD(hh, entry->ppa_table, uintppa, sizeof(uint64_t), ppa_item);  // 내부 맵에 추가
    // printf("MYPRINT| MISS: reached limit(15), so made new entry and added one {%ld, %d} - %ld\n", uintppa, ppa_item->cnt, ppa->ppa);
}

static void *ftl_thread(void *arg);

static inline bool should_gc(struct ssd *ssd) {
    static int last_free_line_cnt = 0;
    if (last_free_line_cnt != ssd->lm.free_line_cnt) {
        last_free_line_cnt = ssd->lm.free_line_cnt;
        printf("MYPRINT| free_line_cnt: %d.(gc when its lower than %d)\n", ssd->lm.free_line_cnt, ssd->sp.gc_thres_lines);
    }
    // 라인이 이 이하로 떨어지면 GC 실행 (75로 설정돼있음. run_blackbox 참고)
    // 슈퍼블록으로 바꿀 경우 가용 슈퍼블록 수로 변경
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd) {
    static int last_free_line_cnt = 0;
    if (last_free_line_cnt != ssd->lm.free_line_cnt) {
        last_free_line_cnt = ssd->lm.free_line_cnt;
        printf("MYPRINT| free_line_cnt: %d.(super gc when its lower than %d)\n", ssd->lm.free_line_cnt, ssd->sp.gc_thres_lines_high);
    }
    // 라인이 이 이하로 떨어지면 GC 실행. (95로 설정돼있음. run_blackbox 참고)
    // 슈퍼블록으로 바꿀 경우 가용 슈퍼블록 수로 변경
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn) {
    // maptable. l2p가 사실상 이건듯.
    // return ssd->maptbl[lpn];
    // l2p로 바꿈
    struct l2p_entry *entry;
    HASH_FIND(hh, ssd->l2p_table, &lpn, sizeof(uint64_t), entry);  // 엔트리 찾기
    if (entry) {
        return entry->ppa;  // PPN 반환
    } else {
        struct ppa empty_ppa;
        empty_ppa.ppa = UNMAPPED_PPA;
        return empty_ppa;  // 엔트리 없음
    }
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa) {
    ftl_assert(lpn < ssd->sp.tt_pgs);
    // ssd->maptbl[lpn] = *ppa;

    struct l2p_entry *entry;
    HASH_FIND(hh, ssd->l2p_table, &lpn, sizeof(uint64_t), entry);  // 기존 엔트리 존재 여부 확인
    if (entry == NULL) {
        entry = (struct l2p_entry *)malloc(sizeof(struct l2p_entry));  // 새 엔트리 생성
        entry->lpn = lpn;
        HASH_ADD(hh, ssd->l2p_table, lpn, sizeof(uint64_t), entry);  // 테이블에 추가
    }
    entry->ppa = *ppa;  // PPN 설정
}

// 이게 ppn인가봄. ppa to index이니깐...
static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa) {
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    // | 4  | 4  | 4  | 4  | 4  | 4  | 4  | 4  | 4  | 4  | 4  | 4  | 4  | 4  | 4  | 4  |
    // ||   ch   |   lun   |    pl   |   sec   |        pg         |        blk        | = ppa
    // ↑rsv

    pgidx = ppa->g.ch * spp->pgs_per_ch      // pgs_per_ch = 256 * 256 * 1 * 8
            + ppa->g.lun * spp->pgs_per_lun  // pgs_per_lun= 256 * 256 * 1
            + ppa->g.pl * spp->pgs_per_pl    // pgs_per_pl = 256 * 256
            + ppa->g.blk * spp->pgs_per_blk  // pgs_per_blk= 256
            + ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    // TODO: 이걸 룬별로 나눠서 해야하는데. 채널은 어떻게 다루고있는지를 모르겠음
    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa) {
    uint64_t pgidx = ppa2pgidx(ssd, ppa);
    // TODO: superblock 별로 있는 페이지테이블로 reverse map 하게 바꾸기
    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa) {
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    // p2l_push(ssd, ppa, lpn);

    // TODO: superblock 별로 있는 페이지테이블에 reverse map 하게 바꾸기
    ssd->rmap[pgidx] = lpn;
}

static void ssd_init_lines(struct ssd *ssd) {
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;
    // TODO: 슈퍼블럭으로 바꿀건데 그럼 lun은 어떻게 할지 생각해야함

    lm->tt_lines = spp->blks_per_pl;  // 한 lun당 256 blk (1룬에 1plane 뿐이니)
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);  // line 수만큼 할당. 근데 이러면 채널은 어떡함??

    // free line list : 일반 큐
    QTAILQ_INIT(&lm->free_line_list);
    // victim line list : 우선순위 큐
    QTAILQ_INIT(&lm->victim_line_list);
    // full line list : 일반 큐
    QTAILQ_INIT(&lm->full_line_list);

    // 초기화하는중...
    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->rpc = 0;
        line->is_victim = false;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    // 만약 저거 두개 다르면 이 이하로는 실행 안되는듯. 걍 if로 하지 왜?
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd) {
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    // free line list에서 poll 해서 curline에 할당
    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    // 첨에 ppa = 0인 상태로 시작
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max) {
    // 변수1이 0이상 변수2 미만인지 체크. 0 <= a < max
    ftl_assert(0 <= a && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd) {
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    // 이것도 그냥 큐에서 뽑아오는거임
    // TODO: 슈퍼블럭으로 바꿀건데 그럼 lun은 어떻게 할지 생각해야함
    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd) {
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);  // 현재 채널이 nchs 이하인지 체크
    wpp->ch++;                       // 다음 채널로 ㄱㄱ
    if (wpp->ch == spp->nchs) {      // 만약 다음 채널이 없다면!! 이제 다음 룬으로 ㄱㄱ
        wpp->ch = 0;                 // 0 -> 1 -> 0 -> 1 -> ... 그 전에 일단 채널은 0으로 초기화

        check_addr(wpp->lun, spp->luns_per_ch);  // 현재 룬이 luns_per_ch 이하인지 체크
        wpp->lun++;                              // 다음 룬으로 ㄱㄱ
        if (wpp->lun == spp->luns_per_ch) {      // 만약 다음 룬이 없다면!! 이제 다음 페이지로 ㄱㄱ
            wpp->lun = 0;                        // 0 -> ... -> 7 -> 0 -> ... -> 7 -> 0 -> ... 그 전에 일단 룬은 0으로 초기화

            check_addr(wpp->pg, spp->pgs_per_blk);  // 현재 페이지가 pgs_per_blk 이하인지 체크
            wpp->pg++;                              // 다음 페이지로 ㄱㄱ
            if (wpp->pg == spp->pgs_per_blk) {      // 만약 다음 페이지가 없다면!! 이제 다음 블록으로 ㄱㄱ
                wpp->pg = 0;                        // 0 -> ... -> 255 -> 0 -> ... -> 255 -> 0 -> ... 그 전에 일단 페이지는 0으로 초기화

                // 이제 블럭ID 바꿀거니 쓰던 라인 상태 바꿔주자. victim / full
                if (wpp->curline->vpc == spp->pgs_per_line) {                      // 만약 현재 라인이 모두 valid page라면
                    ftl_assert(wpp->curline->ipc == 0);                            // 모순 발생! 모두 valid page인데 invalid page가 있다?
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);  // full line list에 넣어주자
                    lm->full_line_cnt++;
                } else {                           // invalid page가 있다면
                    if (wpp->curline->rpc == 0) {  // reference page가 없다면
                        ftl_assert(0 <= wpp->curline->vpc && wpp->curline->vpc < spp->pgs_per_line);
                        ftl_assert(0 < wpp->curline->ipc);
                        QTAILQ_INSERT_TAIL(&lm->victim_line_list, wpp->curline, entry);
                        wpp->curline->is_victim = true;
                        lm->victim_line_cnt++;
                    }
                }

                check_addr(wpp->blk, spp->blks_per_pl);  // 현재 블럭이 blks_per_pl 이하인지 체크... 근데 왜하는거지? 넘겠냐고
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);  // 다음 라인을 가져오자
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }

                wpp->blk = wpp->curline->id;             // 블럭ID를 라인ID로 설정
                check_addr(wpp->blk, spp->blks_per_pl);  // 넘겠냐고

                ftl_assert(wpp->pg == 0);  // 아까 0으로 초기화 했던거 바뀌면 안됨
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(struct ssd *ssd) {
    // 현재 write pointer 위치에다가 쓸거니 그걸 ppa에 넣어서 반환
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp) {
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    // ftl_assert(is_power_of_2(spp->luns_per_ch));
    // ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n) {
    spp->secsz = n->bb_params.secsz;              // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg;  // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk;  // 256
    spp->blks_per_pl = n->bb_params.blks_per_pl;  /* 256 16GB */
    spp->pls_per_lun = n->bb_params.pls_per_lun;  // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch;  // 8
    spp->nchs = n->bb_params.nchs;                // 8

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;  // secs_per_blk : 블록당 섹터 수    = 8 * 256 = 2048
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;  // secs_per_pl  : 플레인당 섹터 수  = 2048 * 256 = 524288
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;  // secs_per_lun : 룬당 섹터 수     = 524288 * 1 = 524288
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;  // secs_per_ch  : 채널당 섹터 수   = 524288 * 8 = 4194304
    spp->tt_secs = spp->secs_per_ch * spp->nchs;              // tt_secs      : 전체 섹터 수     = 4194304 * 2 = 8388608

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;  // pgs_per_pl  : 플레인당 페이지 수 = 256 * 256 = 65536
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;  // pgs_per_lun : 룬당 페이지 수     = 65536 * 1 = 65536
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;  // pgs_per_ch  : 채널당 페이지 수   = 65536 * 8 = 524288
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;              // tt_pgs      : 전체 페이지 수      = 524288 * 2 = 1048576

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;  // blks_per_lun : 룬당 블록 수     = 256 * 1 = 256
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;  // blks_per_ch  : 채널당 블록 수   = 256 * 8 = 2048
    spp->tt_blks = spp->blks_per_ch * spp->nchs;              // tt_blks      : 전체 블록 수     = 2048 * 2 = 4096

    spp->pls_per_ch = spp->pls_per_lun * spp->luns_per_ch;  // pls_per_ch : 채널당 플레인 수 = 1 * 8 = 8
    spp->tt_pls = spp->pls_per_ch * spp->nchs;              // tt_pls     : 전체 플레인 수   = 8 * 2 = 16

    spp->tt_luns = spp->luns_per_ch * spp->nchs;  // tt_luns : 전체 룬 수 = 8 * 2 = 16

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns;
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun;

    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent / 100.0;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high / 100.0;

    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;

    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp) {
    pg->nsecs = spp->secs_per_pg;                                // 한 페이지에는 8개의 섹터가 있음
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);  // 8개의 섹터를 가진 페이지
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;  // 페이지에 있는 섹터들을 모두 FREE로 초기화
    }
    pg->status = PG_FREE;
    pg->rpc = 0;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp) {
    blk->npgs = spp->pgs_per_blk;                               // 한 블록에는 256개의 페이지가 있음
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);  // 블록에 256개의 페이지를 할당
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);  // 블록에 있는 256개의 페이지를 초기화
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->rpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp) {
    pl->nblks = spp->blks_per_pl;                                // 한 플레인에는 256개의 블록이 있음
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);  // 플레인에 256개의 블록을 할당
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);  // 플레인에 있는 256개의 블록을 초기화
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp) {
    lun->npls = spp->pls_per_lun;                                // 한 룬에는 1개의 플레인이 있음
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);  // 룬에 1개의 플레인을 할당
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);  // 룬에 있는 1개의 플레인을 초기화
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;  // <- 이 busy는 뭘까?
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp) {
    ch->nluns = spp->luns_per_ch;                              // 한 채널에는 8개의 룬이 있음
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);  // 채널에 8개의 룬을 할당
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);  // 채널에 있는 8개의 룬을 초기화
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;  // <- 이 busy는 뭘까?
}

static void ssd_init_maptbl(struct ssd *ssd) { ssd->l2p_table = NULL; }

static void ssd_init_rmap(struct ssd *ssd) {
    struct ssdparams *spp = &ssd->sp;

    // ppn -> lpn
    // TODO: superblock 별로 있는 페이지테이블로 reverse map 하게 바꾸기
    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void ssd_init(FemuCtrl *n) {
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);

    ssd_init_params(spp, n);

    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);  // 2개의 채널을 할당
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);  // 채널 초기화
    }

    ssd_init_maptbl(ssd);
    ssd_init_rmap(ssd);

    ssd->bytes_written_from_host = 0;
    ssd->bytes_written_during_gc = 0;

    ssd_init_lines(ssd);
    ssd_init_write_pointer(ssd);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n, QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa) {
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >= 0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg >= 0 && pg < spp->pgs_per_blk && sec >= 0 &&
        sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn) { return (lpn < ssd->sp.tt_pgs); }

static inline bool mapped_ppa(struct ppa *ppa) { return !(ppa->ppa == UNMAPPED_PPA); }

// lun 단위로 시간 계산하는 함수. 건들지 말자
static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd) {
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
        case NAND_READ:
            /* read: perform NAND cmd first */
            nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : lun->next_lun_avail_time;
            lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
            lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
            break;

        case NAND_WRITE:
            /* write: transfer data through channel first */
            nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : lun->next_lun_avail_time;
            if (ncmd->type == USER_IO) {
                lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
            } else {
                lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
            }
            lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
            break;

        case NAND_ERASE:
            // TODO: ppa에서 lpn 정보 받아와서 lpn바꿔주고, sha256 map도 바꿔야됨
            /* erase: only need to advance NAND status */
            nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : lun->next_lun_avail_time;
            lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

            lat = lun->next_lun_avail_time - cmd_stime;
            break;

        default:
            ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_inVALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa) {
    // 페이지를 수정할 때 그거 invalid로 바꾸고 새로 페이지 넣어야 하니깐 invalid로 바꿔주는 함수
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    pg = get_pg(ssd, ppa);               // ppa에 해당하는 페이지를 가져옴
    ftl_assert(pg->status == PG_VALID);  // valid가 아니면 에러
    pg->status = PG_INVALID;             // 페이지 상태를 INVALID로 변경

    blk = get_blk(ssd, ppa);                                   // ppa에 해당하는 블록을 가져옴
    ftl_assert(0 <= blk->ipc && blk->ipc < spp->pgs_per_blk);  // invalid page count를 1 증가시킬 수 없으면 에러
    blk->ipc++;                                                // 블록의 invalid page count를 증가
    ftl_assert(0 < blk->vpc && blk->vpc <= spp->pgs_per_blk);  // valid page count를 1 감소시킬 수 없으면 에러
    blk->vpc--;                                                // 블록의 valid page count를 감소

    line = get_line(ssd, ppa);                                    // ppa에 해당하는 라인을 가져옴
    ftl_assert(0 <= line->ipc && line->ipc < spp->pgs_per_line);  // invalid page count를 1 증가시킬 수 없으면 에러
    if (line->vpc == spp->pgs_per_line) {                         // 모든 페이지가 valid page라면
        ftl_assert(line->ipc == 0);                               // 모든 페이지가 valid면 invalid page count는 무조건 0. 아니면 에러
        was_full_line = true;                                     // was full line을 true로 설정
    }

    line->ipc++;  // 라인의 invalid page count를 증가
    // invalid page count 증가했으니 valid page count는 감소
    ftl_assert(0 < line->vpc && line->vpc <= spp->pgs_per_line);  // valid page count를 1 감소시킬 수 없으면 에러
    line->vpc--;                                                  // line이 victim list에 있으면 그냥 valid page count만 줄여줌

    if (was_full_line) {  // line이 victim list에 없으면
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);         // full line list에서 빼고
        lm->full_line_cnt--;                                     // full line count를 감소
        QTAILQ_INSERT_TAIL(&lm->victim_line_list, line, entry);  // victim line pq에 넣음
        lm->victim_line_cnt++;                                   // victim line count를 증가
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa) {
    // 새로 페이지를 valid하게 넣어주는 함수
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);              // ppa에 해당하는 페이지를 가져옴
    ftl_assert(pg->status == PG_FREE);  // free page가 아니면 에러
    pg->status = PG_VALID;              // 페이지 상태를 VALID로 변경

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);                                      // ppa에 해당하는 블록을 가져옴
    ftl_assert(0 <= blk->vpc && blk->vpc < ssd->sp.pgs_per_blk);  // valid page count를 1 증가시킬 수 없으면 에러
    blk->vpc++;                                                   // 블록의 valid page count를 증가

    /* update corresponding line status */
    line = get_line(ssd, ppa);                                       // ppa에 해당하는 라인을 가져옴
    ftl_assert(0 <= line->vpc && line->vpc < ssd->sp.pgs_per_line);  // valid page count를 1 증가시킬 수 없으면 에러
    line->vpc++;                                                     // 라인의 valid page count를 증가
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa) {
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);  // ppa에 해당하는 블록을 가져옴
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {  // 블럭에 있는 모든 페이지에 대해서
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->rpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa) {
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;  // stime이 0이면 ssd_advance_status에서 현재 시간을 가져와서 사용
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa) {
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);  // old_ppa에 해당하는 lpn을 가져옴

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd);         // 새로 쓰기 위해 페이지 하나 할당 (실제로 메모리에다가 쓰는건 아님. nvme-io가 아님)
    set_maptbl_ent(ssd, lpn, &new_ppa);  // l2p 업데이트
    set_rmap_ent(ssd, lpn, &new_ppa);    // p2l 업데이트

    mark_page_valid(ssd, &new_ppa);  // 새로 할당한 페이지를 valid로 바꿔줌
    ssd_advance_write_pointer(ssd);  // get_new_page 이게 write pointer를 바꿔주지는 않아서 따로 바꿔줌

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;  // stime이 0이면 ssd_advance_status에서 현재 시간을 가져와서 사용
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);                    // 새로 쓴 페이지에 해당하는 룬을 가져옴
    new_lun->gc_endtime = new_lun->next_lun_avail_time;  // 이거 바꾸면 뭐 어케 바뀌는지 모름

    return 0;
}

static struct line *find_victim_line(struct ssd *ssd) {
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;
    struct line *victim_line = NULL;  // vpc가 가장 낮은 line을 저장할 변수

    QTAILQ_FOREACH(line, &lm->victim_line_list, entry) {  // 순회하면서 가장 vpc가 낮은 line을 찾음
        if (victim_line == NULL || line->vpc < victim_line->vpc) {
            victim_line = line;  // 더 낮은 vpc를 가진 line을 업데이트
        }
    }

    return victim_line;  // vpc가 가장 낮은 line을 반환
}

static struct line *select_victim_line(struct ssd *ssd, bool force) {
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    // victim_line = pqueue_peek(lm->victim_line_pq);                           // pq에서 victim line을 가져옴
    victim_line = find_victim_line(ssd);                                     // vpc가 가장 낮은 line을 찾음
    if (!victim_line) return NULL;                                           // pq가 비어있으면 아무것도 안하고 NULL 반환
    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) return NULL;  // force가 아니고 ipc가 32보다 작으면 NULL 반환
    // force가 아니면 invalid가 겨우 32개 이하면 gc 안하려는 듯

    // pqueue_pop(lm->victim_line_pq);  // 이제 진짜 처리 ㄱㄱ pop ㄱㄱ
    QTAILQ_REMOVE(&lm->victim_line_list, victim_line, entry);  // victim line list에서 빼줌
    victim_line->is_victim = false;                            // 왜지? 다시 넣을것도 아닌데 굳이 pos를 0으로 바꾸는 이유가 뭘까? 진짜로 모르겠음
    lm->victim_line_cnt--;                                     // victim line count를 감소

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa) {
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {  // 블록에 있는 모든 페이지에 대해서
        ppa->g.pg = pg;                              // ppa에서 지금 바꾸는건 페이지뿐이니깐 페이지만 계속 바꿔가면서 넣는듯
        pg_iter = get_pg(ssd, ppa);                  // 페이지 숫자 바꾸고 그 숫자에 해당하는 실제 페이지 가지고오기
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);  // free page가 하나라도 있으면 그건 쓸 수 없는 블록이라는 뜻이니깐 에러?
                                                 // 이해는 잘 안간다
        if (pg_iter->status == PG_VALID) {       // valid page면 읽고 쓰면서 수정하는 과정 계산
            gc_read_page(ssd, ppa);
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa) {
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    line->rpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static int do_gc(struct ssd *ssd, bool force) {
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_line = select_victim_line(ssd, force);  // victim line을 선택
    if (!victim_line) return -1;

    printf("MYPRINT|GC: victim_line=%d\n", victim_line->id);

    ppa.g.blk = victim_line->id;  // line 선택했으면 바꿔야할 블럭 번호 자명하니
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk, victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt, ssd->lm.free_line_cnt);

    bool is_freed = true;
    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            // 채널, 룬 바꿔가며 라인에 있는 페이지들 다 처리해야함
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;  // 뭐지? 어차피 plane은 하나니깐 그냥 0 써버린건가?
            lunp = get_lun(ssd, &ppa);

            if (get_block_ref_cnt(ssd, &ppa) > 0) {
                // 사실 ref가 0인애들은 victim line에 들어가지도 않아 여기로 분기되지도 않아서 상관 없음
                printf("MYPRINT| DO_GC: Skipping blk %d, ch %d, lun %d, ref %d\n", ppa.g.blk, ch, lun, get_blk(ssd, &ppa)->rpc);
                is_freed = false;
                continue;
            } 
            // else {
            //     printf("MYPRINT| DO_GC: Processing blk %d, ch %d, lun %d, ref %d\n", ppa.g.blk, ch, lun, get_blk(ssd, &ppa)->rpc);
            // }

            clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    if (is_freed) mark_line_free(ssd, &ppa);  // line에 있는 블럭들 다 처리했으니 line도 free로 바꿔줌

    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req) {
    // printf("MYPRINT| SR: req=%p\n", req);
    // printf("MYPRINT| SR: req->qsg:%p, req->qsg.nsgv:%d\n", (void *)&req->qsg, req->qsg.nsg);
    qemu_sglist_destroy(&req->qsg);  // 원래 dram.c의 코드인데 sha추가하면서 일로 옮김

    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    // req에서는 sector단위로 0부터 최대 sector 개수까지 숫자로 표현해서 어디에 쓸지 보내주나봄
    uint64_t start_lpn = lba / spp->secs_per_pg;              // lba 번호로 역산해서 lpn 번호로 바꿔줌
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;  // nsces / 8 만큼의 페이지를 쓸 것
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) ftl_err("start_lpn=%" PRIu64 ",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);                            // lpn에 해당하는 ppa를 가져옴
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) continue;  // ppa가 unmapped거나 valid한 ppa가 아니면 패스

        // 이건 건드리지 말자
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req) {
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;            // 마찬가지로 lba 번호로 역산해서 lpn 번호로 바꿔줌
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;  // nsces / 8 만큼의 페이지를 쓸 것
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    ssd->bytes_written_from_host += len * spp->secsz;

    if (end_lpn >= spp->tt_pgs) ftl_err("start_lpn=%" PRIu64 ",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);

    while (should_gc_high(ssd)) {  // 95% 이상 쓰여있으면 gc
        r = do_gc(ssd, true);
        if (r == -1) break;  // victim line이 더이상 없으면 break

        ssd->bytes_written_during_gc += spp->secs_per_pg * spp->secsz;  // 운영중에 gc한 총 바이트 수 업데이트
        // printf("MYPRINT| Bytes written during GC: %" PRIu64 "\n", ssd->bytes_written_during_gc);
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);                // lpn에 해당하는 ppa를 가져옴
        if (mapped_ppa(&ppa)) {                        // 이미 매핑된 ppa가 있으면
            if (get_block_ref_cnt(ssd, &ppa) == 0) {   // 그 ppa 소속 블럭에 참조가 블럭이 없어야 invalid 가능
                mark_page_invalid(ssd, &ppa);          // 기존 페이지를 invalid로 바꿔줌
                set_rmap_ent(ssd, INVALID_LPN, &ppa);  // p2l에서 lpn을 invalid로 바꿔줌
            }
        }

        if (could_get_hash_support(req->qsg.hash_array[lpn - start_lpn], req->qsg.hash_len_array[lpn - start_lpn])) {  // 이미 해당 글의 내용이 ppa 어딘가에 있음
            ftl_assert(!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa));
            set_maptbl_ent(ssd, lpn, &ppa);  // ppa에 중복 고려해서 lpn 넣어줌
            add_one_in_hash(ssd, req->qsg.hash_array[lpn - start_lpn], req->qsg.hash_len_array[lpn - start_lpn]);
            // TODO: 슈퍼블럭 로그 추가
        } else {  // 없음. 여긴 슈퍼블럭 부분만 건들면 됨
            ppa = get_new_page(ssd);

            set_maptbl_ent(ssd, lpn, &ppa);  // ppa에 중복 고려해서 lpn 넣어줌
            set_rmap_ent(ssd, lpn, &ppa);

            mark_page_valid(ssd, &ppa);

            map_sha256_to_ppa(req->qsg.hash_array[lpn - start_lpn], req->qsg.hash_len_array[lpn - start_lpn], &ppa);

            ssd_advance_write_pointer(ssd);

            // 이건 건드리지 말자
            struct nand_cmd swr;
            swr.type = USER_IO;
            swr.cmd = NAND_WRITE;
            swr.stime = req->stime;
            /* get latency statistics */
            curlat = ssd_advance_status(ssd, &ppa, &swr);
            maxlat = (curlat > maxlat) ? curlat : maxlat;
        }
    }

    qemu_sglist_destroy(&req->qsg);  // 이거 불리면 qsg free되면서 sha 접근불가.

    print_blk(ssd);

    return maxlat;
}

// femu 시뮬레이션 계산하는 함수인 듯. 이건 건들기 ㄴㄴ
static void *ftl_thread(void *arg) {
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i])) continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            // printf("MYPRINT| FTL: req=%p\n", req);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
                case NVME_CMD_WRITE:
                    lat = ssd_write(ssd, req);
                    break;
                case NVME_CMD_READ:
                    lat = ssd_read(ssd, req);
                    break;
                case NVME_CMD_DSM:
                    lat = 0;
                    break;
                default:
                    // ftl_err("FTL received unkown request type, ERROR\n");
                    ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}
