#include <openssl/evp.h>

#include "../nvme.h"

/* Coperd: FEMU Memory Backend (mbe) for emulated SSD */

int init_dram_backend(SsdDramBackend **mbe, int64_t nbytes) {
    SsdDramBackend *b = *mbe = g_malloc0(sizeof(SsdDramBackend));

    b->size = nbytes;
    b->logical_space = g_malloc0(nbytes);

    if (mlock(b->logical_space, nbytes) == -1) {
        femu_err("Failed to pin the memory backend to the host DRAM\n");
        g_free(b->logical_space);
        abort();
    }

    return 0;
}

void free_dram_backend(SsdDramBackend *b) {
    if (b->logical_space) {
        munlock(b->logical_space, b->size);
        g_free(b->logical_space);
    }
}

int backend_rw(SsdDramBackend *b, QEMUSGList *qsg, uint64_t *lbal, bool is_write) {
    int sg_cur_index = 0;
    dma_addr_t sg_cur_byte = 0;
    dma_addr_t cur_addr, cur_len;
    uint64_t mb_oft = lbal[0];
    void *mb = b->logical_space;

    DMADirection dir = DMA_DIRECTION_FROM_DEVICE;
    if (is_write) {
        dir = DMA_DIRECTION_TO_DEVICE;
    }

    EVP_MD_CTX *mdctx = NULL;
    if (is_write) {
        mdctx = EVP_MD_CTX_new();
        if (mdctx == NULL) {
            femu_err("EVP_MD_CTX_new error\n");
            return -1;
        }

        if (EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL) != 1) {
            femu_err("EVP_DigestInit_ex error\n");
            EVP_MD_CTX_free(mdctx);
            return -1;
        }
    }

    while (sg_cur_index < qsg->nsg) {
        cur_addr = qsg->sg[sg_cur_index].base + sg_cur_byte;
        cur_len = qsg->sg[sg_cur_index].len - sg_cur_byte;

        if (dma_memory_rw(qsg->as, cur_addr, mb + mb_oft, cur_len, dir, MEMTXATTRS_UNSPECIFIED)) {
            femu_err("dma_memory_rw error\n");
        }

        if (is_write) {
            if (EVP_DigestUpdate(mdctx, mb + mb_oft, cur_len) != 1) {
                femu_err("EVP_DigestUpdate error\n");
                EVP_MD_CTX_free(mdctx);
                return -1;
            }
        }

        sg_cur_byte += cur_len;
        if (sg_cur_byte == qsg->sg[sg_cur_index].len) {
            sg_cur_byte = 0;
            ++sg_cur_index;
        }

        if (b->femu_mode == FEMU_OCSSD_MODE) {
            mb_oft = lbal[sg_cur_index];
        } else if (b->femu_mode == FEMU_BBSSD_MODE || b->femu_mode == FEMU_NOSSD_MODE || b->femu_mode == FEMU_ZNSSD_MODE) {
            mb_oft += cur_len;
        } else {
            assert(0);
        }
    }

    if (is_write) {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len = 0;

        if (EVP_DigestFinal_ex(mdctx, hash, &hash_len) != 1) {
            femu_err("EVP_DigestFinal_ex error\n");
            EVP_MD_CTX_free(mdctx);
            return -1;
        }

        memcpy(qsg->sha256, hash, hash_len);

        // 해시 값 출력
        printf("SHA-256 Hash: ");
        for (unsigned int i = 0; i < hash_len; i++) {
            printf("%02x", hash[i]);
        }
        printf("\n");
        printf("SHA-256 Save: ");
        for (unsigned int i = 0; i < hash_len; i++) {
            printf("%02x", qsg->sha256[i]);
        }
        printf("\n");

        // OpenSSL 컨텍스트 정리
        EVP_MD_CTX_free(mdctx);
    }

    // qemu_sglist_destroy(qsg);

    return 0;
}
