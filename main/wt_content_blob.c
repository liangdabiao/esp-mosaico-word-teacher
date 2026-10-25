// main/wt_content_blob.c —— 见 wt_content_blob.h。
//
// 这是整个应用里**唯一**出现链接器 asm 符号名的地方。
#include "wt_content_blob.h"

#include <stdio.h>
#include <string.h>

// 符号名由 main/CMakeLists.txt 里 target_add_binary_data 的**文件名**决定：
//   assets/content/wt_content.bin  ->  _binary_wt_content_bin_start / _end
// 改文件名就必须同时改这里。
//
// 用 asm 标签而不是"定义一个同名数组"：符号定义在链接期由 objcopy 的产物提供，
// 这里只做声明。数组声明后跟 asm 标签在 clang 上不被接受 —— 这也是本文件
// 不参与宿主测试（tests/）的原因：把「字节从哪来」与「怎么解析字节」分开，
// 解析那一层才能在宿主机上跑。
extern const uint8_t wt_content_bytes_start[] asm("_binary_wt_content_bin_start");
extern const uint8_t wt_content_bytes_end[] asm("_binary_wt_content_bin_end");

const uint8_t *wt_content_blob(void) {
    return wt_content_bytes_start;
}

uint32_t wt_content_blob_size(void) {
    return (uint32_t)(wt_content_bytes_end - wt_content_bytes_start);
}

bool wt_content_open(wt_blob_t *out, char *reason, size_t reason_capacity) {
    if (&wt_content_bytes_end == &wt_content_bytes_start || wt_content_blob_size() == 0) {
        // 真机上这一步失败只有一个原因：构建时漏了 target_add_binary_data。
        // 说清楚，省得去查词库文件本身（那个文件是好的）。
        if (reason && reason_capacity) {
            snprintf(reason, reason_capacity, "构建漏了词库附件");
        }
        return false;
    }

    const wt_manifest_err_t manifest_err =
        wt_manifest_open(wt_content_blob(), wt_content_blob_size(), out);
    if (manifest_err != WT_MANIFEST_OK) {
        if (reason && reason_capacity) {
            snprintf(reason, reason_capacity, "%s", wt_manifest_err_text(manifest_err));
        }
        return false;
    }

    uint32_t bad_index = 0;
    const wt_content_err_t content_err = wt_content_verify(out, &bad_index);
    if (content_err != WT_CONTENT_OK) {
        // 带上出错的位置：同一条错误里，"第 3 条就坏了"和"第 19000 条才坏"
        // 一个指向文件头被截断、一个指向字符串池尾部被截断。
        if (reason && reason_capacity) {
            snprintf(reason, reason_capacity, "%s（第 %lu 条）",
                     wt_content_err_text(content_err), (unsigned long)bad_index);
        }
        return false;
    }

    if (reason && reason_capacity) reason[0] = '\0';
    return true;
}
