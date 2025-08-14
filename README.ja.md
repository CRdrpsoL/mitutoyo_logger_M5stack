# mitutoyo_logger_M5stack

M5Stack by M5Stack  3.2.1
ではエラーが発生するために
以下の対処を行った。


C:\Users\ユーザー名\Documents\Arduino\libraries\M5Unified\src\M5Unified.cpp


ファイル先頭付近に以下を追加



#if __has_include("driver/touch_pad.h")
  
#include "driver/touch_pad.h"

#endif