/*
 * presets.h — 键盘菜单预设中文问题 (由 tools/gen_presets.py 生成, 勿手改).
 *
 * 每个预设: 中文原文 (RLCD 显示) + 完整 ChatML token ids (直接推理).
 * ChatML 模板与 tools/send_prompt_rag.py 一致, 无证据纯 Q->A 格式.
 */
#ifndef PRESETS_H
#define PRESETS_H
#include <stdint.h>

typedef struct {
    const char     *text;   // 中文原文 (显示)
    const uint16_t *ids;    // ChatML token ids
    uint8_t         len;    // ids 长度
} KbdPreset;

static const char kbd_preset_0_text[] = "\u80ba\u764c\u7684\u65e9\u671f\u75c7\u72b6\u6709\u54ea\u4e9b";
static const uint16_t kbd_preset_0_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 772, 154, 1469, 270, 296, 3180, 805, 5997, 5371, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_1_text[] = "\u9ad8\u8840\u538b\u7684\u8bca\u65ad\u6807\u51c6\u662f\u4ec0\u4e48";
static const uint16_t kbd_preset_1_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 564, 2789, 1781, 296, 4601, 2441, 3013, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_2_text[] = "\u7cd6\u5c3f\u75c5\u7684\u4e34\u5e8a\u8868\u73b0\u6709\u54ea\u4e9b";
static const uint16_t kbd_preset_2_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 2774, 377, 159, 1756, 296, 2916, 5351, 2660, 5371, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_3_text[] = "\u75c5\u6bd2\u6027\u809d\u708e\u7684\u6cbb\u7597\u539f\u5219\u662f\u4ec0\u4e48";
static const uint16_t kbd_preset_3_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 5022, 470, 772, 287, 5428, 2998, 155, 1554, 4398, 3013, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_4_text[] = "\u611f\u67d3\u6027\u4f11\u514b\u7684\u8840\u8c61\u68c0\u67e5\u6709\u4ec0\u4e48\u7279\u70b9";
static const uint16_t kbd_preset_4_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 6156, 470, 3362, 2047, 296, 2789, 1330, 1715, 4166, 2955, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_5_text[] = "\u611f\u5192\u53d1\u70e7\u5982\u4f55\u6cbb\u7597";
static const uint16_t kbd_preset_5_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 777, 4807, 532, 5508, 1185, 3042, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_6_text[] = "\u6025\u6027\u91cd\u578b\u809d\u708e\u6709\u54ea\u4e9b\u8868\u73b0";
static const uint16_t kbd_preset_6_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 3222, 470, 637, 724, 772, 287, 5428, 5371, 2660, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_7_text[] = "\u7cd6\u5c3f\u75c5\u916e\u75c7\u9178\u4e2d\u6bd2\u600e\u4e48\u529e";
static const uint16_t kbd_preset_7_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 2774, 377, 159, 1756, 950, 142, 3469, 3478, 388, 3437, 4870, 3160, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_8_text[] = "\u5bab\u5916\u5b55\u5982\u4f55\u6cbb\u7597";
static const uint16_t kbd_preset_8_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 5808, 1085, 362, 279, 1185, 3042, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_9_text[] = "\u5e26\u72b6\u75b1\u75b9\u540e\u9057\u795e\u7ecf\u75db\u600e\u4e48\u529e";
static const uint16_t kbd_preset_9_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 1483, 1402, 1000, 145, 1000, 153, 701, 3321, 3707, 4793, 4870, 3160, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_10_text[] = "\u809d\u786c\u5316\u8179\u6c34\u5982\u4f55\u6cbb\u7597";
static const uint16_t kbd_preset_10_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 772, 287, 2992, 511, 2148, 153, 952, 1185, 3042, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_11_text[] = "\u513f\u7ae5\u80ba\u708e\u652f\u539f\u4f53\u611f\u67d3\u600e\u4e48\u5904\u7406";
static const uint16_t kbd_preset_11_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 5657, 772, 154, 5428, 976, 1184, 530, 6156, 4870, 958, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_12_text[] = "\u5fc3\u808c\u6897\u6b7b\u6025\u6551\u63aa\u65bd\u6709\u54ea\u4e9b";
static const uint16_t kbd_preset_12_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 795, 4628, 1789, 281, 5998, 3222, 5093, 2378, 5371, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_13_text[] = "\u767d\u7595\u76ae\u635f\u6709\u4ec0\u4e48\u7279\u70b9";
static const uint16_t kbd_preset_13_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 2178, 1000, 279, 2984, 2894, 4166, 2955, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_14_text[] = "\u9ad8\u8840\u538b\u7528\u836f\u6ce8\u610f\u4e8b\u9879";
static const uint16_t kbd_preset_14_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 564, 2789, 1781, 368, 2381, 1764, 5802, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_15_text[] = "\u809d\u8c46\u72b6\u6838\u53d8\u6027\u662f\u4ec0\u4e48\u75c5";
static const uint16_t kbd_preset_15_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 772, 287, 3948, 1402, 2737, 828, 470, 3013, 1756, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_16_text[] = "\u5931\u7720\u5982\u4f55\u6539\u5584";
static const uint16_t kbd_preset_16_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 1855, 3679, 1185, 3928, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_17_text[] = "\u80c3\u6e83\u75a1\u7684\u996e\u98df\u6ce8\u610f\u4e8b\u9879";
static const uint16_t kbd_preset_17_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 1142, 261, 831, 261, 1000, 130, 296, 3663, 1764, 5802, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_18_text[] = "\u9888\u690e\u75c5\u7684\u9884\u9632\u63aa\u65bd";
static const uint16_t kbd_preset_18_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 431, 266, 1972, 272, 1756, 296, 5220, 2378, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_19_text[] = "\u4e2d\u6691\u7684\u6025\u6551\u65b9\u6cd5";
static const uint16_t kbd_preset_19_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 388, 2005, 275, 296, 3222, 5093, 1064, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_20_text[] = "\u8d2b\u8840\u7684\u539f\u56e0\u6709\u54ea\u4e9b";
static const uint16_t kbd_preset_20_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 471, 140, 2789, 296, 3381, 5371, 2, 234, 1, 1388, 570, 811, 234};

static const char kbd_preset_21_text[] = "\u54ee\u5598\u53d1\u4f5c\u65f6\u600e\u4e48\u529e";
static const uint16_t kbd_preset_21_ids[] = {1, 118, 4849, 234, 441, 1001, 6344, 5127, 294, 960, 1054, 6394, 5136, 5693, 1839, 3854, 746, 302, 2, 234, 1, 832, 311, 234, 746, 337, 494, 142, 1310, 282, 532, 498, 443, 4870, 3160, 2, 234, 1, 1388, 570, 811, 234};

static const KbdPreset kbd_presets[] = {
    {kbd_preset_0_text, kbd_preset_0_ids, 42},
    {kbd_preset_1_text, kbd_preset_1_ids, 40},
    {kbd_preset_2_text, kbd_preset_2_ids, 42},
    {kbd_preset_3_text, kbd_preset_3_ids, 43},
    {kbd_preset_4_text, kbd_preset_4_ids, 43},
    {kbd_preset_5_text, kbd_preset_5_ids, 39},
    {kbd_preset_6_text, kbd_preset_6_ids, 42},
    {kbd_preset_7_text, kbd_preset_7_ids, 45},
    {kbd_preset_8_text, kbd_preset_8_ids, 39},
    {kbd_preset_9_text, kbd_preset_9_ids, 45},
    {kbd_preset_10_text, kbd_preset_10_ids, 42},
    {kbd_preset_11_text, kbd_preset_11_ids, 43},
    {kbd_preset_12_text, kbd_preset_12_ids, 42},
    {kbd_preset_13_text, kbd_preset_13_ids, 40},
    {kbd_preset_14_text, kbd_preset_14_ids, 40},
    {kbd_preset_15_text, kbd_preset_15_ids, 42},
    {kbd_preset_16_text, kbd_preset_16_ids, 37},
    {kbd_preset_17_text, kbd_preset_17_ids, 43},
    {kbd_preset_18_text, kbd_preset_18_ids, 41},
    {kbd_preset_19_text, kbd_preset_19_ids, 40},
    {kbd_preset_20_text, kbd_preset_20_ids, 39},
    {kbd_preset_21_text, kbd_preset_21_ids, 42},
};
#define KBD_PRESET_COUNT 22

#endif
