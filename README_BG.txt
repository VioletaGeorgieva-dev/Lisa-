LISA – AEC synchronization patch
================================

Файлове за проекта:

1) components/command_dtw/lisa_hardware.c
2) components/command_dtw/include/lisa_hardware.h
3) components/command_dtw/CMakeLists.txt
4) main/main.c

Какво е променено:
- RX DMA аудиото, timestamp-ът и sequence номерът се копират заедно в callback-а.
- i2s_channel_read() се използва само за освобождаване/източване на вътрешната
  descriptor queue на ESP-IDF; реалните samples за приложението идват от
  callback-owned queue. Така не се разместват samples и timestamp-и.
- RX application queue drop-овете се възстановяват като синтетични нулеви
  20 ms блокове, така че времевата линия на AEC да не прескача.
- Добавени са on_recv_q_ovf и on_send_q_ovf callbacks и броячи за overflow.
- TX callback копира PCM блока, който току-що е изпратен към I2S.
- Добавена е 32-tap windowed-sinc fractional-delay интерполация 22050 -> 16000 Hz,
  с 128 предварително изчислени фази и проверка за пропуснати TX блокове.
- Добавен е периодичен диагностичен лог за RX/TX queue drops, driver overflows,
  липсващо reference покритие и максимално изчакване на reference.
- Добавена е директна зависимост esp_timer в command_dtw CMakeLists.txt.

Запазени са GPIO, SD mount point, WAV playback API, команди, FSM и AEC API.

ПРОВЕРКИ:
- lisa_hardware.c премина host C syntax check с -Wall -Wextra -Werror срещу
  минимални ESP-IDF API mocks.
- FIR коефициентите са проверени числено за нормировка (сумата е ~1 за всички
  фази). Това не е хардуерен тест.
- main.c и hardware не са компилирани тук с ESP-IDF 5.5.1; след замяната
  потребителят трябва да изпълни само `idf.py build`.

ВАЖНО:
- НЕ изпълнявай `idf.py flash`.
- НЕ изпълнявай `erase-flash`.
- НЕ променяй partitions.csv.
- НЕ добавяй espressif__esp-sr към REQUIRES.
