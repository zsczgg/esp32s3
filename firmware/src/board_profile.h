/**
 * 板型配置 —— 屏幕和触摸的引脚全在这里，换板只改这一个文件
 *
 * ============ 怎么用 ============
 * 1) 先选预置：把下面的 BOARD_PRESET 改成对应的一款
 * 2) 预置不对就选 PRESET_CUSTOM，自己填下面的引脚
 * 3) 编译烧录后：白屏 → 引脚或驱动型号错；画面颜色反了 → INVERT_COLOR 改 1；
 *    画面偏色/红蓝互换 → RGB_ORDER 改 1
 *
 * ============ 怎么知道你板子的引脚 ============
 * 看板子背面丝印，或屏排线上的字（常见 "2.8 TFT SPI 240x320" / "ST7796"），
 * 再对照厂家的引脚表。国产这类板子最常见的几种接法我已经预置进去了。
 */
#pragma once

// ============================================================
// 选一款
// ============================================================
#define PRESET_ILI9341_28   1   // 2.8 寸 320x240，ILI9341 + XPT2046 电阻触摸（最常见）
#define PRESET_ST7796_35    2   // 3.5 寸 480x320，ST7796 + XPT2046 电阻触摸
#define PRESET_ST7789_24    3   // 2.4 寸 320x240，ST7789 + 无触摸
#define PRESET_S3_28_FT     4   // 2.8 寸 240x320 ST7789 + FT6236（按原厂寄存器推断）
#define PRESET_S3_28_ALT    5   // 同上但沿用"能点亮"的那组引脚（DC=46 RST=9 BL=45 CS=10）
#define PRESET_S3_28_DC18   6
#define PRESET_S3_28_DC18_BL10 7  // DC=18 + 背光=10   // 严格按原厂寄存器：SCLK12/MOSI11 无CS 无RST，DC=18，背光交给 0x18 扩展器
#define PRESET_S3_28_LCDWIKI 8    // ★ 官方 LCDWiki「2.8inch ESP32-S3 Display」原厂引脚，实测用这个
#define PRESET_CUSTOM       99  // 自己填

// ★★★ 改这里 ★★★
#ifndef BOARD_PRESET
#define BOARD_PRESET  PRESET_S3_28_LCDWIKI
#endif

// ============================================================
// 预置区（一般不用动）
// ============================================================
#if BOARD_PRESET == PRESET_ILI9341_28
  #define LCD_DRV_ILI9341   1
  #define LCD_W             320
  #define LCD_H             240
  #define LCD_ROTATION      1        // 0/2=竖屏  1/3=横屏；3 是 1 的镜像
  #define LCD_SPI_HZ        40000000 // 白屏或花屏先降到 20000000 试
  #define PIN_LCD_SCLK      12
  #define PIN_LCD_MOSI      11
  #define PIN_LCD_MISO      13
  #define PIN_LCD_CS        10
  #define PIN_LCD_DC        46
  #define PIN_LCD_RST       9        // -1 = 和板子复位脚共用
  #define PIN_LCD_BL        45       // -1 = 背光常亮
  #define LCD_BL_PWM        1        // 1=支持亮度调节  0=只能开/关
  #define LCD_INVERT_COLOR  0
  #define LCD_RGB_ORDER     0

  #define TOUCH_XPT2046     1
  #define PIN_TP_CS         47
  #define PIN_TP_IRQ        48       // -1 = 轮询
  #define PIN_TP_SCLK       12       // 与屏共用 SPI 时填一样的
  #define PIN_TP_MOSI       11
  #define PIN_TP_MISO       13
  #define TP_SPI_HZ         2500000
  #define TP_X_MIN          300      // 电阻触摸校准值，点不准就调这四个
  #define TP_X_MAX          3900
  #define TP_Y_MIN          300
  #define TP_Y_MAX          3900
  #define TP_SWAP_XY        0
  #define TP_INVERT_X       0
  #define TP_INVERT_Y       0

#elif BOARD_PRESET == PRESET_ST7796_35
  #define LCD_DRV_ST7796    1
  #define LCD_W             480
  #define LCD_H             320
  #define LCD_ROTATION      1
  #define LCD_SPI_HZ        40000000
  #define PIN_LCD_SCLK      12
  #define PIN_LCD_MOSI      11
  #define PIN_LCD_MISO      13
  #define PIN_LCD_CS        10
  #define PIN_LCD_DC        46
  #define PIN_LCD_RST       9
  #define PIN_LCD_BL        45
  #define LCD_BL_PWM        1
  #define LCD_INVERT_COLOR  0
  #define LCD_RGB_ORDER     1

  #define TOUCH_XPT2046     1
  #define PIN_TP_CS         47
  #define PIN_TP_IRQ        48
  #define PIN_TP_SCLK       12
  #define PIN_TP_MOSI       11
  #define PIN_TP_MISO       13
  #define TP_SPI_HZ         2500000
  #define TP_X_MIN          300
  #define TP_X_MAX          3900
  #define TP_Y_MIN          300
  #define TP_Y_MAX          3900
  #define TP_SWAP_XY        0
  #define TP_INVERT_X       0
  #define TP_INVERT_Y       0

#elif BOARD_PRESET == PRESET_ST7789_24
  #define LCD_DRV_ST7789    1
  #define LCD_W             320
  #define LCD_H             240
  #define LCD_ROTATION      1
  #define LCD_SPI_HZ        40000000
  #define PIN_LCD_SCLK      12
  #define PIN_LCD_MOSI      11
  #define PIN_LCD_MISO      (-1)     // ST7789 一般不接 MISO
  #define PIN_LCD_CS        10
  #define PIN_LCD_DC        46
  #define PIN_LCD_RST       9
  #define PIN_LCD_BL        45
  #define LCD_BL_PWM        1
  #define LCD_INVERT_COLOR  0
  #define LCD_RGB_ORDER     0

  #define TOUCH_NONE        1

#elif BOARD_PRESET == PRESET_S3_28_FT
  // ============================================================
  // 本机实测（原厂固件运行时用 OpenOCD 读 GPIO 寄存器得出）
  //   2026-09-16 交接文档 + 2026-11-24 MAC 3C:0F:02:DD:B7:5C 同一台
  //   屏 SCLK=GPIO12  MOSI=GPIO11  无 MISO（只写不读）
  //   全片没有 CS 脚 → CS 硬件接地，LGFX 填 -1
  //   全片只有 GPIO10 / GPIO18 被使能成普通输出 → DC 和背光就是这两个
  //   触摸是 FT6236 电容触摸，挂在 I2C：SCL=15  SDA=16，地址 0x38
  // ============================================================
  #define LCD_DRV_ST7789    1
  #define LCD_W             240
  #define LCD_H             320
  #define LCD_ROTATION      1
  #define LCD_SPI_HZ        40000000
  #define PIN_LCD_SCLK      12
  #define PIN_LCD_MOSI      11
  #define PIN_LCD_MISO      (-1)
  #define PIN_LCD_CS        (-1)     // CS 焊死接地，不能用 GPIO
  #define PIN_LCD_DC        10       // 可能与 18 互换，见文件末尾的覆盖宏
  #define PIN_LCD_RST       (-1)     // 未知，交给软复位
  #define PIN_LCD_BL        18
  #define LCD_BL_PWM        1
  #define LCD_INVERT_COLOR  1        // IPS 屏一般要反色，反了改 0
  #define LCD_RGB_ORDER     0

  #define TOUCH_FT5X06      1
  #define PIN_TP_SDA        16
  #define PIN_TP_SCL        15
  #define PIN_TP_IRQ        (-1)
  #define PIN_TP_RST        (-1)

#elif BOARD_PRESET == PRESET_S3_28_ALT
  // 沿用第一轮能点亮屏幕的那组引脚，只把驱动 IC 换成 ST7789
  #define LCD_DRV_ST7789    1
  #define LCD_W             240
  #define LCD_H             320
  #define LCD_ROTATION      1
  #define LCD_SPI_HZ        40000000
  #define PIN_LCD_SCLK      12
  #define PIN_LCD_MOSI      11
  #define PIN_LCD_MISO      13
  #define PIN_LCD_CS        10
  #define PIN_LCD_DC        46
  #define PIN_LCD_RST       9
  #define PIN_LCD_BL        45
  #define LCD_BL_PWM        1
  #define LCD_INVERT_COLOR  1
  #define LCD_RGB_ORDER     0

  #define TOUCH_FT5X06      1
  #define PIN_TP_SDA        16
  #define PIN_TP_SCL        15
  #define PIN_TP_IRQ        (-1)
  #define PIN_TP_RST        (-1)

#elif BOARD_PRESET == PRESET_S3_28_DC18
  // 完全按原厂固件的寄存器转储来：
  //   SCLK=12  MOSI=11  无 MISO  无 CS（硬件接地）  无 RST
  //   全片只有 GPIO10 / GPIO18 是普通输出 → 10 试过全黑，这里用 18
  //   背光不走 GPIO（22 个脚扫过都没反应）→ 交给 I2C 上那颗 0x18 扩展器
  #define LCD_DRV_ST7789    1
  #define LCD_W             240
  #define LCD_H             320
  #define LCD_ROTATION      1
  #define LCD_SPI_HZ        40000000
  #define PIN_LCD_SCLK      12
  #define PIN_LCD_MOSI      11
  #define PIN_LCD_MISO      (-1)
  #define PIN_LCD_CS        (-1)
  #define PIN_LCD_DC        18
  #define PIN_LCD_RST       (-1)
  #define PIN_LCD_BL        (-1)     // 背光由 0x18 扩展器管，不占 GPIO
  #define LCD_BL_PWM        0
  #define LCD_INVERT_COLOR  1
  #define LCD_RGB_ORDER     0

  #define TOUCH_FT5X06      1
  #define PIN_TP_SDA        16
  #define PIN_TP_SCL        15
  #define PIN_TP_IRQ        (-1)
  #define PIN_TP_RST        (-1)

#elif BOARD_PRESET == PRESET_S3_28_DC18_BL10
  #define LCD_DRV_ST7789    1
  #define LCD_W             240
  #define LCD_H             320
  #define LCD_ROTATION      1
  #define LCD_SPI_HZ        40000000
  #define PIN_LCD_SCLK      12
  #define PIN_LCD_MOSI      11
  #define PIN_LCD_MISO      (-1)
  #define PIN_LCD_CS        (-1)
  #define PIN_LCD_DC        18
  #define PIN_LCD_RST       (-1)
  #define PIN_LCD_BL        10
  #define LCD_BL_PWM        1
  #define LCD_INVERT_COLOR  1
  #define LCD_RGB_ORDER     0

  #define TOUCH_FT5X06      1
  #define PIN_TP_SDA        16
  #define PIN_TP_SCL        15
  #define PIN_TP_IRQ        (-1)
  #define PIN_TP_RST        (-1)

#elif BOARD_PRESET == PRESET_S3_28_LCDWIKI
  // ============================================================
  // 官方：LCDWiki「2.8inch ESP32-S3 Display」ES3C28P / ES3N28P
  //   https://www.lcdwiki.com/zh/2.8inch_ESP32-S3_Display
  //   屏：2.8" IPS TFT 240x320，驱动 ILI9341V，4 线 SPI
  //   触摸：FT6336G 电容，I2C
  //
  //   LCD  IO10=CS(低有效)  IO46=DC(高=数据)  IO12=SCLK  IO11=MOSI  IO13=MISO
  //        RST 与 ESP32 的 EN 共用 → 填 -1，由芯片复位统一带
  //        IO45=背光（高电平点亮）
  //   触摸 IO16=SDA  IO15=SCL  IO18=RST(低复位)  IO17=INT(有触摸时低)
  //   其它 IO1=音频使能(低开) IO42=RGB灯 IO38~48=SD卡 IO9=电池ADC
  //
  //   ⚠ 之前 I2C 扫到的 0x18 不是 IO 扩展器，是 ES8311 音频解码芯片，
  //     别往它写寄存器。
  // ============================================================
  #define LCD_DRV_ILI9341   1
  #define LCD_W             240      // 面板原生宽（竖屏）
  #define LCD_H             320      // 面板原生高
  #define LCD_ROTATION      1        // 1 = 横屏 320x240（界面按横屏排版）
  #define LCD_SPI_HZ        40000000
  #define PIN_LCD_SCLK      12
  #define PIN_LCD_MOSI      11
  #define PIN_LCD_MISO      13
  #define PIN_LCD_CS        10
  #define PIN_LCD_DC        46
  #define PIN_LCD_RST       (-1)     // EN 共用，不需要单独驱动
  #define PIN_LCD_BL        45
  #define LCD_BL_PWM        1        // IO45 走 LEDC 调光；点不亮就换 s3-lcd-wiki-plain
  #define LCD_INVERT_COLOR  1        // 本机实测：色块测试发现 0 会整体反色，正确值=1
                                     // 以测试页「反色」按钮现场试为准
  #define LCD_RGB_ORDER     0

  #define TOUCH_FT5X06      1
  #define PIN_TP_SDA        16
  #define PIN_TP_SCL        15
  #define PIN_TP_IRQ        17
  #define PIN_TP_RST        18
  #define TP_I2C_ADDR       0x38     // FT6336G 7 位地址
  #define TP_OFFSET_ROT     2        // 触摸坐标补偿：等效 r=(1+2)&3=3，即交换XY+X翻转
                                     // 只有 r 为奇数（带 XY 交换）坐标范围才对得上 320x240

#elif BOARD_PRESET == PRESET_CUSTOM
  // ---- 自己填 ----
  #define LCD_DRV_ILI9341   1        // ILI9341 / ST7796 / ST7789 选一个
  #define LCD_W             320
  #define LCD_H             240
  #define LCD_ROTATION      1
  #define LCD_SPI_HZ        40000000
  #define PIN_LCD_SCLK      12
  #define PIN_LCD_MOSI      11
  #define PIN_LCD_MISO      13
  #define PIN_LCD_CS        10
  #define PIN_LCD_DC        46
  #define PIN_LCD_RST       9
  #define PIN_LCD_BL        45
  #define LCD_BL_PWM        1
  #define LCD_INVERT_COLOR  0
  #define LCD_RGB_ORDER     0

  #define TOUCH_NONE        1        // 改成 TOUCH_XPT2046 / TOUCH_CST816 / TOUCH_GT911
  #define PIN_TP_CS         47
  #define PIN_TP_IRQ        48
  #define PIN_TP_SDA        18       // 电容触摸用 I2C
  #define PIN_TP_SCL        19
  #define PIN_TP_RST        (-1)
#endif

// ============================================================
// 下面不用改
// ============================================================
#if !defined(TOUCH_XPT2046) && !defined(TOUCH_CST816) && \
    !defined(TOUCH_GT911)   && !defined(TOUCH_FT5X06) && !defined(TOUCH_NONE)
  #define TOUCH_NONE 1
#endif

#ifdef TOUCH_NONE
  #define HAS_TOUCH  0
#else
  #define HAS_TOUCH  1
#endif

// 竖屏时把宽高换过来（LVGL 用旋转后的尺寸）
#if (LCD_ROTATION == 1) || (LCD_ROTATION == 3)
  #define SCREEN_W  ((LCD_W) > (LCD_H) ? (LCD_W) : (LCD_H))
  #define SCREEN_H  ((LCD_W) > (LCD_H) ? (LCD_H) : (LCD_W))
#else
  #define SCREEN_W  ((LCD_W) < (LCD_H) ? (LCD_W) : (LCD_H))
  #define SCREEN_H  ((LCD_W) < (LCD_H) ? (LCD_H) : (LCD_W))
#endif

// 允许用编译参数临时改 SPI 速度，不用动这个文件：
//   pio run -e s3-lcd -DLCD_SPI_HZ_SET=20000000 ...
#ifdef LCD_SPI_HZ_SET
  #undef LCD_SPI_HZ
  #define LCD_SPI_HZ  LCD_SPI_HZ_SET
#endif

// DC / 背光 / 反色 也可以从编译参数覆盖，用来不动文件试组合：
//   -DPIN_LCD_DC_SET=18 -DPIN_LCD_BL_SET=10 -DLCD_INVERT_SET=0
#ifdef PIN_LCD_DC_SET
  #undef PIN_LCD_DC
  #define PIN_LCD_DC  PIN_LCD_DC_SET
#endif
#ifdef PIN_LCD_BL_SET
  #undef PIN_LCD_BL
  #define PIN_LCD_BL  PIN_LCD_BL_SET
#endif
#ifdef LCD_INVERT_SET
  #undef LCD_INVERT_COLOR
  #define LCD_INVERT_COLOR  LCD_INVERT_SET
#endif
#ifdef LCD_ROTATION_SET
  #undef LCD_ROTATION
  #define LCD_ROTATION  LCD_ROTATION_SET
#endif
