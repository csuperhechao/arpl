/*
 * 
 */

/**
 * Converted from php code by Fabio Belavenuto <belavenuto@gmail.com>
 * 
 * A quick tool for patching the boot_params check in the DSM kernel image
 * This lets you tinker with the initial ramdisk contents without disabling mount() features and modules loading
 *
 * The overall pattern we need to find is:
 *  - an CDECL function
 *  - does "LOCK OR [const-ptr],n" 4x
 *  - values of ORs are 1/2/4/8 respectively
 *  - [const-ptr] is always the same
 *
 */
/**
 * A quick tool for patching the ramdisk check in the DSM kernel image
 * This lets you tinker with the initial ramdisk contents without disabling mount() features and modules loading
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <gelf.h>

const int DIR_FWD = 1;
const int DIR_RWD = -1;

/* Variables */
int fd;
int verbose = 1, read_only = 0;
Elf       *elfHandle;
GElf_Ehdr elfExecHeader;
uint64_t  orPos[4], fileSize, rodataAddr, rodataOffs, initTextOffs;
unsigned char *fileData;

/*****************************************************************************/
void errorMsg(char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

/*****************************************************************************/
void errorNum() {
    char str[100] = {0};
    perror(str);
    exit(2);
}

/*****************************************************************************/
void elfErrno() {
   int err;

    if ((err = elf_errno()) != 0) {
        fprintf(stderr, "%s\n", elf_errmsg(err));
        exit(3);
    }
}

/*****************************************************************************/
//Finding a function boundary is non-trivial really as patters can vary, we can have multiple exit points, and in CISC
// there are many things which may match e.g. "PUSH EBP". Implementing even a rough disassembler is pointless.
//However, we can certainly cheat here as we know with CDECL a non-empty function will always contain one or more
// PUSH (0x41) R12-R15 (0x54-57) sequences. Then we can search like a 1K forward for these characteristic LOCK OR.
uint64_t findPUSH_R12_R15_SEQ(int size, uint64_t start) {
    uint64_t i;

    for (i = start; i < size; i++) {
        if (fileData[i] == 0x41 && (fileData[i+1] >= 0x54 && fileData[i+1] <= 0x57)) {
            return i;
        }
    }
    return -1;
}

/*****************************************************************************/
//[0xF0, 0x80, null, null, null, null, null, 0xXX],
uint64_t findORs(unsigned char *data, int size, uint64_t start) {
    uint64_t i;
    int c = 0;

    for (i = start; i < size; i++) {
        if (data[i] == 0xF0 && data[i+1] == 0x80) {
            if (data[i+7] == 0x01 || data[i+7] == 0x02 || data[i+7] == 0x04 || data[i+7] == 0x08) {
                orPos[c++] = i;
                i += 8;
            }
        }
        if (c == 4)
            break;
    }
    return c;
}

/*****************************************************************************/
void patchBootParams() {
    uint64_t addr, pos;
    uint64_t newPtrOffset, ptrOffset;
    int n;

    //The function will reside in init code part. We don't care we may potentially search beyond as we expect it to be found

    while (initTextOffs < fileSize) {
        addr = findPUSH_R12_R15_SEQ(fileSize, initTextOffs);
        if (addr == -1)
            break; //no more "functions" left
        printf("\rAnalyzing f() candidate @ %lX", addr);
        //we found something looking like PUSH R12-R15, now find the ORs
        n = findORs(fileData+initTextOffs, 1024, 0);
        if (n != 4) {
            //We can always move forward by the function token length (obvious) but if we couldn't find any LOCK-OR tokens
            // we can skip the whole look ahead distance. We CANNOT do that if we found even a single token because the next one
            // might have been just after the look ahead distance
            initTextOffs = addr + 2;
            if (n == 0) {
                initTextOffs += 1024;
            }
            continue; //Continue the main search loop to find next function candidate
        }
        //We found LOCK(); OR ptr sequences so we can print some logs and collect ptrs (as this is quite expensive)
        printf("\n[?] Found possible f() @ %lX\n", initTextOffs);
        ptrOffset=0;
        int ec = 0;
        for (n = 0; n < 4; n++) {
            //data will have the following bytes:
            // [0-LOCK()] [1-OR()] [2-BYTE-PTR] [3-OFFS-b3] [4-OFFS-b2] [5-OFFS-b1] [6-OFFS-b1] [7-NUMBER]
            pos = initTextOffs + orPos[n];
            //how far it "jumps"
            newPtrOffset = pos + (fileData[pos+6] << 24 | fileData[pos+5] << 16 | fileData[pos+4] << 8 | fileData[pos+3]);
            if (ptrOffset == 0) {
                ptrOffset = newPtrOffset;
                ++ec;
            } else if (ptrOffset == newPtrOffset) {
                ++ec;
            }
            printf("\t[+] Found LOCK-OR#$idx sequence @ %lX => %02X %02X %02X %02X %02X %02X %02X %02X [RIP+%lX]\n",
              pos, fileData[pos], fileData[pos+1], fileData[pos+2], fileData[pos+3], fileData[pos+4],
              fileData[pos+5], fileData[pos+6], fileData[pos+7], newPtrOffset);
        }
        if (ec != 4) {
            printf("\t[-] LOCK-OR PTR offset mismatch - %d/8 matched\n", ec);
            //If the pointer checking failed we can at least move beyond the last LOCK-OR found as we know there's no valid
            // sequence of LOCK-ORs there
            initTextOffs += orPos[3];
            continue;
        }
        printf("\t[+] All %d LOCK-OR PTR offsets equal - match found!\n", ec);
        break;
    }
    if (initTextOffs >= fileSize) {
        printf("Failed to find matching sequences");
    } else {
        //Patch offsets
        for (n = 0; n < 4; n++) {
            //The offset will point at LOCK(), we need to change the OR (0x80 0x0d) to AND (0x80 0x25) so the two bytes after
            pos = initTextOffs + orPos[n] + 2;
            printf("Patching OR to AND @ %lX\n", pos);
            fileData[pos] = 0x25;
        }
    }
}

/*****************************************************************************/
uint32_t changeEndian(uint32_t num) {
    return ((num>>24)&0xff)       | // move byte 3 to byte 0
           ((num<<8)&0xff0000)    | // move byte 1 to byte 2
           ((num>>8)&0xff00)      | // move byte 2 to byte 1
           ((num<<24)&0xff000000);  // move byte 0 to byte 3
}

/*****************************************************************************/
uint64_t findSeq(const char* seq, int len, uint32_t pos, int dir, uint64_t max) {
    uint64_t i;

    i = pos;
    do {
        if (strncmp((const char*)fileData+i, seq, len) == 0) {
            return i;
        }
        i += dir;
        --max;
    } while(i > 0 && i < fileSize && max > 0);
    return -1;
}

/*****************************************************************************/
void patchRamdiskCheck() {
    uint64_t pos, errPrintAddr;
    uint64_t printkPos, testPos, jzPos;
    const char str[] = "3ramdisk corrupt";

    printf("Patching ramdisk check\n");
    for (pos = rodataOffs; pos < fileSize; pos++) {
        if (strncmp(str, (const char*)(fileData + pos), 16) == 0) {
            pos -= rodataOffs;
            break;
        }
    }
    errPrintAddr = rodataAddr + pos - 1;
    printf("LE arg addr: %08lX\n", errPrintAddr);
    printkPos = findSeq((const char*)&errPrintAddr, 4, 0, DIR_FWD, -1);
    if (printkPos == -1) {
        errorMsg("printk pos not found!");
    }
    //double check if it's a MOV reg,VAL (where reg is EAX/ECX/EDX/EBX/ESP/EBP/ESI/EDI)
    printkPos -= 3;
    if (strncmp((const char*)fileData+printkPos, "\x48\xc7", 2) != 0) {
        printf("Expected MOV=>reg before printk error, got %02X %02X\n", fileData[printkPos], fileData[printkPos+1]);
        errorMsg("");
    }
    if (fileData[printkPos+2] < 0xC0 || fileData[printkPos+2] > 0xC7) {
        printf("Expected MOV w/reg operand [C0-C7], got %02X\n", fileData[printkPos+2]);
        errorMsg("");
    }
    printf("Found printk MOV @ %08lX\n", printkPos);

    //now we should seek a reasonable amount (say, up to 32 bytes) for a sequence of CALL x => TEST EAX,EAX => JZ
    testPos = findSeq("\x85\xc0", 2, printkPos, DIR_RWD, 32);
    if (testPos == -1) {
        errorMsg("Failed to find TEST eax,eax\n");
    }
    printf("Found TEST eax,eax @ %08lX\n", testPos);
    jzPos = testPos + 2;
    if (fileData[jzPos] != 0x74) {
        errorMsg("Failed to find JZ\n");
    }
    printf("OK - patching %02X%02X (JZ) to %02X%02X (JMP) @ %08lX\n", 
      fileData[jzPos], fileData[jzPos+1], 0xEB, fileData[jzPos+1], jzPos);
    fileData[jzPos] = 0xEB;
}

/*****************************************************************************/
int main(int argc, char *argv[]) {
    struct stat fileInf;
    Elf_Scn *section;
    GElf_Shdr sectionHeader;
    char *sectionName;

    if (argc != 2) {
        errorMsg("You must specify an elf file to patch");
    }

    if (elf_version(EV_CURRENT) == EV_NONE)
        elfErrno();

    if ((fd = open(argv[1], O_RDWR)) == -1)
        errorNum();

    if ((elfHandle = elf_begin(fd, ELF_C_READ, NULL)) == NULL)
        elfErrno();
    if (gelf_getehdr(elfHandle, &elfExecHeader) == NULL)
        elfErrno();

    switch(elf_kind(elfHandle)) {
        case ELF_K_NUM:
        case ELF_K_NONE:
            errorMsg("file type unknown");
            break;
        case ELF_K_COFF:
            errorMsg("COFF binaries not supported");
            break;
        case ELF_K_AR:
            errorMsg("AR archives not supported");
            break;
        case ELF_K_ELF:
            break;
    }

    section = NULL;
    while ((section = elf_nextscn(elfHandle, section)) != NULL) {
        if (gelf_getshdr(section, &sectionHeader) != &sectionHeader)
            elfErrno();
        if ((sectionName = elf_strptr(elfHandle, elfExecHeader.e_shstrndx, sectionHeader.sh_name)) == NULL)
            elfErrno();
        if (strcmp(sectionName, ".init.text") == 0) {
            initTextOffs = sectionHeader.sh_offset;
        } else if (strcmp(sectionName, ".rodata") == 0) {
            rodataAddr = sectionHeader.sh_addr & 0xFFFFFFFF;
            rodataOffs = sectionHeader.sh_offset;
        }
    }
    elfErrno(); /* If there isn't elf_errno set, nothing will happend. */
    elf_end(elfHandle);

    if (fstat(fd, &fileInf) == -1)
        errorNum();

    fileSize = fileInf.st_size;
    if ((fileData = mmap(NULL, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
        errorNum();

    patchBootParams();
    patchRamdiskCheck();

    munmap(fileData, fileSize);

    close(fd);
    printf("\n");
}
