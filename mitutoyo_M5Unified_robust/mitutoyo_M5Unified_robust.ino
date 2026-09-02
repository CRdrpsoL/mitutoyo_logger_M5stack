#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Mitutoyo Digimatic logger for M5Stack Basic.
// CSV format: No.,measurement (one record per line)

M5Canvas canvas(&M5.Display);

constexpr int REQ_PIN = 26;
constexpr int DATA_PIN = 17;
constexpr int CLK_PIN = 16;
constexpr int RECORD_TRIGGER_PIN = 36;
constexpr int SD_CS_PIN = GPIO_NUM_4;

constexpr uint32_t SD_FREQUENCY_HZ = 25000000;
constexpr uint32_t CALIPER_EDGE_TIMEOUT_US = 100000;
constexpr uint32_t LOOP_DELAY_MS = 100;
constexpr uint32_t BATTERY_UPDATE_INTERVAL_MS = 5000;

// ADC hysteresis: pressed <= 100, released >= 300.
constexpr int RECORD_PRESS_THRESHOLD = 100;
constexpr int RECORD_RELEASE_THRESHOLD = 300;
constexpr uint32_t RECORD_REARM_MS = 150;

constexpr uint32_t DELETE_HOLD_MS = 1000;
constexpr uint8_t CSV_DECIMAL_PLACES = 2;

// false prevents an accidental switch to inch mode from mixing units in the CSV.
constexpr bool ALLOW_INCH_VALUES = false;

constexpr char DATA_FILE[] = "/nashi.csv";
constexpr char TEMP_FILE[] = "/nashi.tmp";
constexpr char BACKUP_FILE[] = "/nashi.bak";
constexpr char JOURNAL_FILE[] = "/nashi.jrn";
constexpr char JOURNAL_TEMP_FILE[] = "/nashi.jrt";

constexpr size_t CSV_LINE_BUFFER_SIZE = 96;
constexpr size_t RECORD_BUFFER_SIZE = 64;
constexpr uint32_t JOURNAL_MAGIC = 0x4D354A31UL;  // "M5J1"
constexpr uint32_t JOURNAL_VERSION = 1;

struct Measurement {
  double value;
  uint8_t unit;  // 0: mm, 1: inch
};

enum class CaliperReadStatus : uint8_t {
  Ok,
  Timeout,
  InvalidFrame,
  InchNotAllowed,
};

struct AppendJournal {
  uint32_t magic;
  uint32_t version;
  uint32_t baseSize;
  uint32_t recordLength;
  uint32_t checksum;
  char record[RECORD_BUFFER_SIZE];
};

static_assert(sizeof(AppendJournal) == 84, "Unexpected journal layout");

uint32_t nextNo = 1;
uint32_t recordCount = 0;
double sumValue = 0.0;
double averageValue = 0.0;

bool recordTriggerPressed = false;
bool recordTriggerArmed = true;
uint32_t triggerReleaseStart = 0;

uint32_t btnAPressStart = 0;
bool btnALongPressHandled = false;

int lastValidBatteryLevel = -1;
uint32_t lastBatteryReadTime = 0;
bool batteryReadStarted = false;

// -----------------------------------------------------------------------------
// Small file helpers
// -----------------------------------------------------------------------------

bool removeIfExists(const char* path) {
  return !SD.exists(path) || SD.remove(path);
}

bool writeAll(File& file, const uint8_t* data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    const size_t written = file.write(data + offset, length - offset);
    if (written == 0) return false;
    offset += written;
  }
  return true;
}

bool readExact(File& file, uint8_t* data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    const int bytesRead = file.read(data + offset, length - offset);
    if (bytesRead <= 0) return false;
    offset += static_cast<size_t>(bytesRead);
  }
  return true;
}

bool getDataFileSize(uint32_t& sizeOut) {
  if (!SD.exists(DATA_FILE)) {
    sizeOut = 0;
    return true;
  }

  File file = SD.open(DATA_FILE, FILE_READ);
  if (!file) return false;
  sizeOut = static_cast<uint32_t>(file.size());
  file.close();
  return true;
}

bool verifyWholeFile(const char* path, const uint8_t* expected, size_t length) {
  File file = SD.open(path, FILE_READ);
  if (!file) return false;

  bool ok = (file.size() == length);
  uint8_t buffer[sizeof(AppendJournal)];
  if (length > sizeof(buffer)) ok = false;
  if (ok) ok = readExact(file, buffer, length);
  file.close();

  return ok && memcmp(buffer, expected, length) == 0;
}

bool verifyDataTail(uint32_t baseSize, const char* record, size_t recordLength) {
  if (recordLength > UINT32_MAX || baseSize > UINT32_MAX - recordLength) return false;

  File file = SD.open(DATA_FILE, FILE_READ);
  if (!file) return false;

  const uint32_t expectedSize = baseSize + static_cast<uint32_t>(recordLength);
  bool ok = (file.size() == expectedSize);
  if (ok) ok = file.seek(baseSize);

  char readBack[RECORD_BUFFER_SIZE];
  if (recordLength > sizeof(readBack)) ok = false;
  if (ok) {
    ok = readExact(file, reinterpret_cast<uint8_t*>(readBack), recordLength);
  }
  file.close();

  return ok && memcmp(readBack, record, recordLength) == 0;
}

bool copyPrefixToTemp(uint32_t bytesToKeep) {
  File input = SD.open(DATA_FILE, FILE_READ);
  if (!input) return false;
  if (input.size() < bytesToKeep) {
    input.close();
    return false;
  }

  if (!removeIfExists(TEMP_FILE)) {
    input.close();
    return false;
  }

  File output = SD.open(TEMP_FILE, FILE_WRITE);
  if (!output) {
    input.close();
    return false;
  }

  uint8_t buffer[512];
  uint32_t remaining = bytesToKeep;
  bool ok = true;
  while (remaining > 0) {
    const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const int bytesRead = input.read(buffer, wanted);
    if (bytesRead <= 0 || !writeAll(output, buffer, static_cast<size_t>(bytesRead))) {
      ok = false;
      break;
    }
    remaining -= static_cast<uint32_t>(bytesRead);
  }

  output.flush();
  input.close();
  output.close();

  if (ok) {
    File verify = SD.open(TEMP_FILE, FILE_READ);
    ok = verify && verify.size() == bytesToKeep;
    if (verify) verify.close();
  }

  if (!ok) removeIfExists(TEMP_FILE);
  return ok;
}

bool replaceDataFileWithTemp() {
  if (!SD.exists(TEMP_FILE)) return false;
  if (!removeIfExists(BACKUP_FILE)) return false;

  const bool hadDataFile = SD.exists(DATA_FILE);
  if (hadDataFile && !SD.rename(DATA_FILE, BACKUP_FILE)) return false;

  if (!SD.rename(TEMP_FILE, DATA_FILE)) {
    if (hadDataFile && SD.exists(BACKUP_FILE) && !SD.exists(DATA_FILE)) {
      SD.rename(BACKUP_FILE, DATA_FILE);
    }
    return false;
  }

  if (!SD.exists(DATA_FILE)) return false;

  // DATA_FILE is already valid here. A leftover backup is recoverable on reboot.
  if (SD.exists(BACKUP_FILE) && !SD.remove(BACKUP_FILE)) {
    Serial.println("Warning: could not remove CSV backup");
  }
  return true;
}

bool rollbackFileToSize(uint32_t originalSize) {
  if (!SD.exists(DATA_FILE)) return originalSize == 0;

  uint32_t currentSize = 0;
  if (!getDataFileSize(currentSize)) return false;
  if (currentSize == originalSize) return true;
  if (currentSize < originalSize) return false;

  return copyPrefixToTemp(originalSize) && replaceDataFileWithTemp();
}

bool recoverInterruptedFileReplacement() {
  bool hasData = SD.exists(DATA_FILE);
  bool hasBackup = SD.exists(BACKUP_FILE);

  if (!hasData && hasBackup) {
    if (!SD.rename(BACKUP_FILE, DATA_FILE)) return false;
    hasData = true;
    hasBackup = false;
  }

  if (hasData && hasBackup && !SD.remove(BACKUP_FILE)) return false;
  if (SD.exists(TEMP_FILE) && !SD.remove(TEMP_FILE)) return false;
  return true;
}

// -----------------------------------------------------------------------------
// Append journal and power-loss recovery
// -----------------------------------------------------------------------------

uint32_t fnv1aUpdate(uint32_t hash, const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t calculateJournalChecksum(const AppendJournal& journal) {
  uint32_t hash = 2166136261UL;
  hash = fnv1aUpdate(hash, reinterpret_cast<const uint8_t*>(&journal.magic), sizeof(journal.magic));
  hash = fnv1aUpdate(hash, reinterpret_cast<const uint8_t*>(&journal.version), sizeof(journal.version));
  hash = fnv1aUpdate(hash, reinterpret_cast<const uint8_t*>(&journal.baseSize), sizeof(journal.baseSize));
  hash = fnv1aUpdate(hash, reinterpret_cast<const uint8_t*>(&journal.recordLength), sizeof(journal.recordLength));
  hash = fnv1aUpdate(hash, reinterpret_cast<const uint8_t*>(journal.record), journal.recordLength);
  return hash;
}

bool validateJournal(const AppendJournal& journal) {
  return journal.magic == JOURNAL_MAGIC &&
         journal.version == JOURNAL_VERSION &&
         journal.recordLength > 0 &&
         journal.recordLength <= sizeof(journal.record) &&
         journal.checksum == calculateJournalChecksum(journal);
}

bool createAppendJournal(uint32_t baseSize, const char* record, size_t recordLength) {
  if (recordLength == 0 || recordLength > RECORD_BUFFER_SIZE) return false;
  if (!removeIfExists(JOURNAL_TEMP_FILE)) return false;
  if (SD.exists(JOURNAL_FILE)) return false;

  AppendJournal journal;
  memset(&journal, 0, sizeof(journal));
  journal.magic = JOURNAL_MAGIC;
  journal.version = JOURNAL_VERSION;
  journal.baseSize = baseSize;
  journal.recordLength = static_cast<uint32_t>(recordLength);
  memcpy(journal.record, record, recordLength);
  journal.checksum = calculateJournalChecksum(journal);

  File file = SD.open(JOURNAL_TEMP_FILE, FILE_WRITE);
  if (!file) return false;
  bool ok = writeAll(file, reinterpret_cast<const uint8_t*>(&journal), sizeof(journal));
  file.flush();
  file.close();

  if (ok) {
    ok = verifyWholeFile(JOURNAL_TEMP_FILE,
                         reinterpret_cast<const uint8_t*>(&journal), sizeof(journal));
  }
  if (ok) ok = SD.rename(JOURNAL_TEMP_FILE, JOURNAL_FILE);
  if (ok) {
    ok = verifyWholeFile(JOURNAL_FILE,
                         reinterpret_cast<const uint8_t*>(&journal), sizeof(journal));
  }

  if (!ok) removeIfExists(JOURNAL_TEMP_FILE);
  return ok;
}

bool loadAppendJournal(AppendJournal& journal) {
  File file = SD.open(JOURNAL_FILE, FILE_READ);
  if (!file) return false;
  bool ok = file.size() == sizeof(journal);
  if (ok) ok = readExact(file, reinterpret_cast<uint8_t*>(&journal), sizeof(journal));
  file.close();
  return ok && validateJournal(journal);
}

bool recoverInterruptedAppend() {
  if (SD.exists(JOURNAL_TEMP_FILE) && !SD.remove(JOURNAL_TEMP_FILE)) return false;
  if (!SD.exists(JOURNAL_FILE)) return true;

  AppendJournal journal;
  if (!loadAppendJournal(journal)) {
    // A final journal is only installed after a complete verified temporary write.
    // If it is nevertheless corrupt, retain/repair the CSV by its own valid rows.
    Serial.println("Warning: invalid append journal");
    return SD.remove(JOURNAL_FILE);
  }

  if (!SD.exists(DATA_FILE)) {
    if (journal.baseSize != 0) return false;
    return SD.remove(JOURNAL_FILE);
  }

  uint32_t currentSize = 0;
  if (!getDataFileSize(currentSize)) return false;

  if (currentSize == journal.baseSize) {
    return SD.remove(JOURNAL_FILE);
  }

  if (journal.baseSize > UINT32_MAX - journal.recordLength) return false;
  const uint32_t expectedSize = journal.baseSize + journal.recordLength;
  if (currentSize == expectedSize &&
      verifyDataTail(journal.baseSize, journal.record, journal.recordLength)) {
    // The complete record reached the CSV before power was lost: commit it.
    return SD.remove(JOURNAL_FILE);
  }

  if (currentSize < journal.baseSize) return false;
  if (!rollbackFileToSize(journal.baseSize)) return false;
  return SD.remove(JOURNAL_FILE);
}

// -----------------------------------------------------------------------------
// CSV parsing, startup restoration, and tail repair
// -----------------------------------------------------------------------------

bool parseCsvRecord(char* line, uint32_t& noOut, double& valueOut) {
  char* cursor = line;
  while (isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  if (*cursor == '\0') return false;

  errno = 0;
  char* numberEnd = nullptr;
  const unsigned long parsedNo = strtoul(cursor, &numberEnd, 10);
  if (errno == ERANGE || numberEnd == cursor || parsedNo == 0 || parsedNo > UINT32_MAX) {
    return false;
  }

  cursor = numberEnd;
  while (isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  if (*cursor != ',') return false;
  ++cursor;
  while (isspace(static_cast<unsigned char>(*cursor))) ++cursor;

  errno = 0;
  char* valueEnd = nullptr;
  const double parsedValue = strtod(cursor, &valueEnd);
  if (errno == ERANGE || valueEnd == cursor || !isfinite(parsedValue)) return false;

  cursor = valueEnd;
  while (isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  if (*cursor != '\0') return false;

  noOut = static_cast<uint32_t>(parsedNo);
  valueOut = parsedValue;
  return true;
}

bool repairTrailingCsvCorruption() {
  if (!SD.exists(DATA_FILE)) return true;

  File file = SD.open(DATA_FILE, FILE_READ);
  if (!file) return false;

  const uint32_t fileSize = static_cast<uint32_t>(file.size());
  uint32_t offset = 0;
  uint32_t lastValidEnd = 0;
  char line[CSV_LINE_BUFFER_SIZE];
  size_t lineLength = 0;
  bool overflow = false;

  while (file.available()) {
    const int input = file.read();
    if (input < 0) break;
    ++offset;

    if (input == '\n') {
      line[lineLength] = '\0';
      uint32_t parsedNo = 0;
      double parsedValue = 0.0;
      if (!overflow && parseCsvRecord(line, parsedNo, parsedValue)) {
        lastValidEnd = offset;
      }
      lineLength = 0;
      overflow = false;
    } else if (input == 0) {
      overflow = true;
    } else if (!overflow) {
      if (lineLength + 1 < sizeof(line)) {
        line[lineLength++] = static_cast<char>(input);
      } else {
        overflow = true;
      }
    }
  }

  if (lineLength > 0 || overflow) {
    line[lineLength] = '\0';
    uint32_t parsedNo = 0;
    double parsedValue = 0.0;
    if (!overflow && parseCsvRecord(line, parsedNo, parsedValue)) {
      lastValidEnd = offset;
    }
  }

  bool nonWhitespaceTail = false;
  if (lastValidEnd < fileSize) {
    if (!file.seek(lastValidEnd)) {
      file.close();
      return false;
    }
    while (file.available()) {
      const int input = file.read();
      if (input >= 0 && !isspace(static_cast<unsigned char>(input))) {
        nonWhitespaceTail = true;
        break;
      }
    }
  }
  file.close();

  if (!nonWhitespaceTail) return true;
  Serial.println("Repairing invalid trailing CSV data");
  return rollbackFileToSize(lastValidEnd);
}

void resetSessionState() {
  nextNo = 1;
  recordCount = 0;
  sumValue = 0.0;
  averageValue = 0.0;
}

bool addRecordToLatestSession(uint32_t parsedNo, double parsedValue,
                              bool& sessionActive, uint32_t& lastSessionNo) {
  if (parsedNo == 1) {
    recordCount = 1;
    sumValue = parsedValue;
    sessionActive = true;
    lastSessionNo = 1;
    return isfinite(sumValue);
  }

  if (!sessionActive || parsedNo != lastSessionNo + 1) {
    // A later valid No.1 is required to identify the start of a session.
    resetSessionState();
    sessionActive = false;
    lastSessionNo = 0;
    return true;
  }

  if (recordCount == UINT32_MAX) return false;
  ++recordCount;
  sumValue += parsedValue;
  lastSessionNo = parsedNo;
  return isfinite(sumValue);
}

bool loadLatestSessionState() {
  resetSessionState();

  if (!SD.exists(DATA_FILE)) return true;

  File file = SD.open(DATA_FILE, FILE_READ);
  if (!file) return false;

  uint32_t lastSessionNo = 0;
  bool sessionActive = false;
  char line[CSV_LINE_BUFFER_SIZE];
  size_t lineLength = 0;
  bool overflow = false;

  while (file.available()) {
    const int input = file.read();
    if (input < 0) break;

    if (input == '\n') {
      line[lineLength] = '\0';
      uint32_t parsedNo = 0;
      double parsedValue = 0.0;
      if (!overflow && parseCsvRecord(line, parsedNo, parsedValue)) {
        if (!addRecordToLatestSession(parsedNo, parsedValue,
                                      sessionActive, lastSessionNo)) {
          file.close();
          return false;
        }
      }
      lineLength = 0;
      overflow = false;
    } else if (input == 0) {
      overflow = true;
    } else if (!overflow) {
      if (lineLength + 1 < sizeof(line)) {
        line[lineLength++] = static_cast<char>(input);
      } else {
        overflow = true;
      }
    }
  }

  if (lineLength > 0 || overflow) {
    line[lineLength] = '\0';
    uint32_t parsedNo = 0;
    double parsedValue = 0.0;
    if (!overflow && parseCsvRecord(line, parsedNo, parsedValue)) {
      if (!addRecordToLatestSession(parsedNo, parsedValue,
                                    sessionActive, lastSessionNo)) {
        file.close();
        return false;
      }
    }
  }
  file.close();

  if (!sessionActive) {
    resetSessionState();
    return true;
  }
  if (lastSessionNo == UINT32_MAX) return false;
  nextNo = lastSessionNo + 1;
  averageValue = recordCount == 0 ? 0.0 : sumValue / static_cast<double>(recordCount);
  return isfinite(sumValue) && isfinite(averageValue);
}

bool ensureTrailingNewline() {
  if (!SD.exists(DATA_FILE)) return true;

  File input = SD.open(DATA_FILE, FILE_READ);
  if (!input) return false;
  const uint32_t originalSize = static_cast<uint32_t>(input.size());
  if (originalSize == 0) {
    input.close();
    return true;
  }

  bool ok = input.seek(originalSize - 1);
  const int lastByte = ok ? input.read() : -1;
  input.close();
  if (lastByte == '\n') return true;

  File output = SD.open(DATA_FILE, FILE_APPEND);
  if (!output) return false;
  const uint8_t newline = '\n';
  ok = output.write(&newline, 1) == 1;
  output.flush();
  output.close();

  File verify = SD.open(DATA_FILE, FILE_READ);
  if (!verify) return false;
  ok = ok && verify.size() == originalSize + 1 && verify.seek(originalSize) && verify.read() == '\n';
  verify.close();
  return ok;
}

bool appendCurrentRecordVerified(double measuredValue) {
  if (!isfinite(measuredValue) || nextNo == UINT32_MAX || recordCount == UINT32_MAX) {
    return false;
  }

  // Clear a rare stale journal before creating the next transaction.
  if (SD.exists(JOURNAL_FILE)) {
    if (!recoverInterruptedAppend() ||
        !repairTrailingCsvCorruption() ||
        !loadLatestSessionState()) {
      return false;
    }
  }

  if (!ensureTrailingNewline()) return false;

  uint32_t baseSize = 0;
  if (!getDataFileSize(baseSize)) return false;

  char record[RECORD_BUFFER_SIZE];
  const int recordLength = snprintf(record, sizeof(record), "%lu,%.*f\n",
                                    static_cast<unsigned long>(nextNo),
                                    CSV_DECIMAL_PLACES, measuredValue);
  if (recordLength <= 0 || static_cast<size_t>(recordLength) >= sizeof(record)) return false;
  if (baseSize > UINT32_MAX - static_cast<uint32_t>(recordLength)) return false;

  if (!createAppendJournal(baseSize, record, static_cast<size_t>(recordLength))) return false;

  File file = SD.open(DATA_FILE, FILE_APPEND);
  bool writeOk = false;
  if (file) {
    writeOk = writeAll(file, reinterpret_cast<const uint8_t*>(record),
                       static_cast<size_t>(recordLength));
    file.flush();
    file.close();
  }

  const bool verified = writeOk &&
      verifyDataTail(baseSize, record, static_cast<size_t>(recordLength));
  if (!verified) {
    const bool rolledBack = rollbackFileToSize(baseSize);
    if (rolledBack) removeIfExists(JOURNAL_FILE);
    return false;
  }

  // The CSV is the source of truth. If journal cleanup fails, startup/next press
  // will recognize this exact verified record as committed.
  if (!removeIfExists(JOURNAL_FILE)) {
    Serial.println("Warning: append committed but journal cleanup failed");
  }

  sumValue += measuredValue;
  ++recordCount;
  averageValue = sumValue / static_cast<double>(recordCount);
  ++nextNo;
  return true;
}

bool findLastNonEmptyLineStart(uint32_t& lineStartOut) {
  File file = SD.open(DATA_FILE, FILE_READ);
  if (!file) return false;

  uint32_t offset = 0;
  uint32_t currentLineStart = 0;
  uint32_t lastNonEmptyStart = 0;
  bool currentLineHasContent = false;
  bool found = false;

  while (file.available()) {
    const int input = file.read();
    if (input < 0) break;
    ++offset;

    if (input == '\n') {
      if (currentLineHasContent) {
        lastNonEmptyStart = currentLineStart;
        found = true;
      }
      currentLineStart = offset;
      currentLineHasContent = false;
    } else if (!isspace(static_cast<unsigned char>(input))) {
      currentLineHasContent = true;
    }
  }

  if (currentLineHasContent) {
    lastNonEmptyStart = currentLineStart;
    found = true;
  }
  file.close();

  if (found) lineStartOut = lastNonEmptyStart;
  return found;
}

bool readCsvRecordAt(uint32_t lineStart, uint32_t& noOut, double& valueOut) {
  File file = SD.open(DATA_FILE, FILE_READ);
  if (!file) return false;
  if (!file.seek(lineStart)) {
    file.close();
    return false;
  }

  char line[CSV_LINE_BUFFER_SIZE];
  size_t lineLength = 0;
  bool overflow = false;
  while (file.available()) {
    const int input = file.read();
    if (input < 0 || input == '\n') break;
    if (input == 0 || lineLength + 1 >= sizeof(line)) {
      overflow = true;
      continue;
    }
    if (!overflow) line[lineLength++] = static_cast<char>(input);
  }
  file.close();

  if (overflow) return false;
  line[lineLength] = '\0';
  return parseCsvRecord(line, noOut, valueOut);
}

bool deleteLastRecord() {
  const bool hadJournal = SD.exists(JOURNAL_FILE);
  if (!recoverInterruptedAppend() || !repairTrailingCsvCorruption()) return false;
  if (hadJournal && !loadLatestSessionState()) return false;

  // Do not delete records belonging to a previous power-on session.
  if (recordCount == 0 || nextNo <= 1) return false;
  if (!SD.exists(DATA_FILE)) return false;

  uint32_t lastLineStart = 0;
  if (!findLastNonEmptyLineStart(lastLineStart)) return false;

  uint32_t deletedNo = 0;
  double deletedValue = 0.0;
  if (!readCsvRecordAt(lastLineStart, deletedNo, deletedValue)) return false;
  if (deletedNo != nextNo - 1) return false;

  if (!copyPrefixToTemp(lastLineStart)) return false;
  if (!replaceDataFileWithTemp()) return false;

  --nextNo;
  --recordCount;
  sumValue -= deletedValue;
  if (recordCount == 0) {
    sumValue = 0.0;
    averageValue = 0.0;
  } else {
    averageValue = sumValue / static_cast<double>(recordCount);
  }
  return isfinite(sumValue) && isfinite(averageValue);
}

// -----------------------------------------------------------------------------
// Mitutoyo Digimatic input
// -----------------------------------------------------------------------------

bool waitWhilePinState(int pin, int state, uint32_t timeoutUs) {
  const uint32_t start = micros();
  uint32_t lastYield = start;

  while (digitalRead(pin) == state) {
    const uint32_t now = micros();
    if (static_cast<uint32_t>(now - start) >= timeoutUs) return false;
    // A normal edge arrives in a few hundred microseconds. Yield only when the
    // signal is already abnormally slow, so normal bit timing is not disturbed.
    if (static_cast<uint32_t>(now - lastYield) >= 1000) {
      yield();
      lastYield = micros();
    }
  }
  return true;
}

CaliperReadStatus decodeDigimaticFrame(const uint8_t data[13], Measurement& measurement) {
  for (int i = 0; i < 4; ++i) {
    if (data[i] != 0x0F) return CaliperReadStatus::InvalidFrame;
  }

  const uint8_t sign = data[4];
  if (sign != 0 && sign != 8) return CaliperReadStatus::InvalidFrame;

  uint32_t magnitude = 0;
  for (int i = 5; i <= 10; ++i) {
    if (data[i] > 9) return CaliperReadStatus::InvalidFrame;
    magnitude = magnitude * 10 + data[i];
  }

  const uint8_t decimalPlaces = data[11];
  const uint8_t unit = data[12];
  if (decimalPlaces > 5 || unit > 1) return CaliperReadStatus::InvalidFrame;
  if (!ALLOW_INCH_VALUES && unit == 1) return CaliperReadStatus::InchNotAllowed;

  double decoded = static_cast<double>(magnitude);
  for (uint8_t i = 0; i < decimalPlaces; ++i) decoded /= 10.0;
  if (sign == 8) decoded = -decoded;
  if (!isfinite(decoded)) return CaliperReadStatus::InvalidFrame;

  measurement.value = decoded;
  measurement.unit = unit;
  return CaliperReadStatus::Ok;
}

CaliperReadStatus readCaliper(Measurement& measurement) {
  uint8_t data[13] = {0};
  bool ok = true;

  digitalWrite(REQ_PIN, HIGH);
  for (int nibble = 0; nibble < 13 && ok; ++nibble) {
    uint8_t value = 0;
    for (int bit = 0; bit < 4; ++bit) {
      if (!waitWhilePinState(CLK_PIN, LOW, CALIPER_EDGE_TIMEOUT_US) ||
          !waitWhilePinState(CLK_PIN, HIGH, CALIPER_EDGE_TIMEOUT_US)) {
        ok = false;
        break;
      }
      bitWrite(value, bit, digitalRead(DATA_PIN) & 0x01);
    }
    data[nibble] = value;
  }
  digitalWrite(REQ_PIN, LOW);

  if (!ok) return CaliperReadStatus::Timeout;
  return decodeDigimaticFrame(data, measurement);
}

// -----------------------------------------------------------------------------
// Trigger, display, and sound
// -----------------------------------------------------------------------------

bool updateRecordTrigger() {
  const int adc = analogRead(RECORD_TRIGGER_PIN);

  if (recordTriggerPressed) {
    if (adc >= RECORD_RELEASE_THRESHOLD) recordTriggerPressed = false;
  } else if (adc <= RECORD_PRESS_THRESHOLD) {
    recordTriggerPressed = true;
  }

  bool pressEvent = false;
  if (recordTriggerPressed) {
    triggerReleaseStart = 0;
    if (recordTriggerArmed) {
      recordTriggerArmed = false;
      pressEvent = true;
    }
  } else if (!recordTriggerArmed) {
    if (triggerReleaseStart == 0) triggerReleaseStart = millis();
    if (static_cast<uint32_t>(millis() - triggerReleaseStart) >= RECORD_REARM_MS) {
      recordTriggerArmed = true;
      triggerReleaseStart = 0;
    }
  }

  return pressEvent;
}

int getStableBatteryLevel() {
  const uint32_t now = millis();
  if (!batteryReadStarted ||
      static_cast<uint32_t>(now - lastBatteryReadTime) >= BATTERY_UPDATE_INTERVAL_MS) {
    batteryReadStarted = true;
    lastBatteryReadTime = now;

    const int reading = M5.Power.getBatteryLevel();
    if (reading >= 0 && reading <= 100) {
      lastValidBatteryLevel = reading;
    }
  }
  return lastValidBatteryLevel;
}

void drawBattery(int level) {
  canvas.setTextSize(3);
  canvas.setCursor(230, 20);
  if (level >= 0 && level <= 100) {
    canvas.printf("%3d%%", level);
  } else {
    canvas.print(" --%");
  }
}

void showMeasurement(const Measurement& measurement, int batteryLevel) {
  canvas.fillScreen(TFT_BLACK);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);

  canvas.setTextSize(3);
  canvas.setCursor(10, 20);
  canvas.print("count:");
  canvas.setCursor(140, 20);
  canvas.printf("%lu", static_cast<unsigned long>(nextNo));
  drawBattery(batteryLevel);

  canvas.setTextSize(6);
  canvas.setCursor(45, 95);
  canvas.printf("%7.2f", measurement.value);

  canvas.setTextSize(4);
  canvas.setCursor(40, 175);
  canvas.print("Avr:");
  canvas.setCursor(140, 175);
  canvas.printf("%7.2f", averageValue);
  canvas.pushSprite(0, 0);
}

void showMessage(const char* message, uint32_t color = TFT_WHITE) {
  canvas.fillScreen(TFT_BLACK);
  canvas.setTextColor(color, TFT_BLACK);
  canvas.setTextSize(5);
  canvas.setTextDatum(textdatum_t::middle_center);
  canvas.drawString(message, canvas.width() / 2, canvas.height() / 2);
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.pushSprite(0, 0);
}

void playOkSound() {
  M5.Speaker.setVolume(100);
  M5.Speaker.tone(2637, 300);
  delay(300);
  M5.Speaker.stop();
}

void playErrorSound() {
  M5.Speaker.setVolume(50);
  M5.Speaker.tone(500, 300);
  delay(300);
  M5.Speaker.stop();
}

void playDeleteSound() {
  M5.Speaker.setVolume(100);
  M5.Speaker.tone(440, 400);
  delay(400);
  M5.Speaker.stop();
}

bool handleDeleteButton() {
  if (M5.BtnA.isPressed()) {
    if (btnAPressStart == 0) btnAPressStart = millis();
    if (!btnALongPressHandled &&
        static_cast<uint32_t>(millis() - btnAPressStart) >= DELETE_HOLD_MS) {
      btnALongPressHandled = true;
      if (deleteLastRecord()) {
        showMessage("DEL", TFT_YELLOW);
        playDeleteSound();
      } else {
        showMessage("X", TFT_RED);
        playErrorSound();
      }
      return true;
    }
  } else {
    btnAPressStart = 0;
    btnALongPressHandled = false;
  }
  return false;
}

bool initializeStorageState() {
  if (!recoverInterruptedFileReplacement() ||
      !recoverInterruptedAppend() ||
      !repairTrailingCsvCorruption()) {
    return false;
  }

  // Every power-on or reset starts a new measurement session. Existing CSV
  // records remain on the SD card, but numbering and the average restart here.
  resetSessionState();
  return true;
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  M5.begin(cfg);
  M5.Power.begin();

  pinMode(REQ_PIN, OUTPUT);
  pinMode(CLK_PIN, INPUT_PULLUP);
  pinMode(DATA_PIN, INPUT_PULLUP);
  pinMode(RECORD_TRIGGER_PIN, INPUT);
  digitalWrite(REQ_PIN, LOW);

  canvas.setColorDepth(8);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);

  while (!SD.begin(SD_CS_PIN, SPI, SD_FREQUENCY_HZ)) {
    showMessage("SD WAIT", TFT_RED);
    delay(500);
  }

  while (!initializeStorageState()) {
    showMessage("SD ERROR", TFT_RED);
    playErrorSound();
    delay(1000);
  }

  // If the switch is already down at boot, require a release before arming.
  const int initialAdc = analogRead(RECORD_TRIGGER_PIN);
  recordTriggerPressed = initialAdc < RECORD_RELEASE_THRESHOLD;
  recordTriggerArmed = !recordTriggerPressed;
}

void loop() {
  M5.update();

  const bool recordPressEvent = updateRecordTrigger();
  const bool deleteHandled = handleDeleteButton();

  Measurement measurement = {0.0, 0};
  const CaliperReadStatus readStatus = readCaliper(measurement);

  if (readStatus != CaliperReadStatus::Ok) {
    if (readStatus == CaliperReadStatus::InchNotAllowed) {
      showMessage("MM ONLY", TFT_ORANGE);
    } else if (readStatus == CaliperReadStatus::InvalidFrame) {
      showMessage("DATA ERROR", TFT_ORANGE);
    } else {
      showMessage("NO DEVICE", TFT_ORANGE);
    }
    if (recordPressEvent && !deleteHandled) playErrorSound();
    delay(LOOP_DELAY_MS);
    return;
  }

  showMeasurement(measurement, getStableBatteryLevel());

  if (recordPressEvent && !deleteHandled) {
    if (appendCurrentRecordVerified(measurement.value)) {
      showMessage("OK", TFT_GREEN);
      playOkSound();
    } else {
      showMessage("X", TFT_RED);
      playErrorSound();
    }
  }

  delay(LOOP_DELAY_MS);
}
