/* M5Avatar の顔を出し、SanoTTS-jp で喋りながら口をリップシンクさせるサンプル。
 *
 *   起動時に 1 文喋り、以後は画面をタッチするたびに次の文を喋る。
 *
 * 仕組み:
 *   SanoTTSSpeakerM5 を継承した LipSyncSpeaker がスピーカーへ流す PCM を横取りし、
 *   チャンク(2,048 sample = 93 ms)ごとの RMS を「再生開始からのサンプル位置」つきで
 *   記録する。loop() 側で「今鳴っているチャンク」の値を拾って
 *   avatar.setMouthOpenRatio() に渡す。
 *
 *   tts.say() は喋り終わるまでブロックするので、TTS は別タスク(core 0)で回し、
 *   loop()(core 1)は M5.update() と口の更新だけをする。
 *
 * 辞書が焼けていないと kanjiReady() が false になり漢字は読めないので、
 * その場合はかな中間表現で喋る。 */
#include <Arduino.h>
#include <M5Unified.h>
#include <Avatar.h>
#include <SanoTTS.h>
#include <SanoTTSSpeakerM5.h>

using namespace m5avatar;

/* 口の開き具合 = RMS / kMouthRefRms を 0..1 にクリップ。
 * 実機で口の開きが小さすぎ/大きすぎなら、ここを調整する
 * (シリアルに出る absmax を目安にする)。 */
static const float kMouthRefRms = 2500.0f;

class LipSyncSpeaker : public SanoTTSSpeakerM5 {
public:
  bool beginUtterance(size_t n) override {
    m_playing = false;
    m_head = m_tail = 0;
    m_written = 0;
    return SanoTTSSpeakerM5::beginUtterance(n);
  }
  bool prerollPush(const int16_t* pcm, size_t n) override {
    push(pcm, n);
    return SanoTTSSpeakerM5::prerollPush(pcm, n);
  }
  bool start() override {
    m_t0 = millis();
    m_playing = true;
    return SanoTTSSpeakerM5::start();
  }
  bool write(const int16_t* pcm, size_t n) override {
    push(pcm, n);                       /* write() はキューが空くまでブロックするので先に記録 */
    return SanoTTSSpeakerM5::write(pcm, n);
  }
  void stop() override {
    SanoTTSSpeakerM5::stop();           /* 鳴り終わるまで待ってから */
    m_playing = false;
  }

  /* 今この瞬間に鳴っているチャンクの口の開き具合 (0..1)。loop() から呼ぶ。 */
  float mouthOpenRatio() {
    if (!m_playing) return 0.0f;
    const uint32_t pos = (uint32_t)((uint64_t)(millis() - m_t0) * SanoTTS::kSampleRate / 1000);
    while (m_head != m_tail && m_ring[m_head].end <= pos) m_head = (m_head + 1) % kRing;
    if (m_head == m_tail || m_ring[m_head].start > pos) return 0.0f;
    return m_ring[m_head].level;
  }

private:
  struct Entry { uint32_t start, end; float level; };
  /* 書き手(TTS タスク)は再生より数チャンク先行するだけなので 32 で十分 */
  static const size_t kRing = 32;

  void push(const int16_t* pcm, size_t n) {
    uint64_t sq = 0;
    for (size_t i = 0; i < n; ++i) sq += (int32_t)pcm[i] * (int32_t)pcm[i];
    const float rms = n ? sqrtf((float)sq / (float)n) : 0.0f;
    float level = rms / kMouthRefRms;
    if (level > 1.0f) level = 1.0f;

    const size_t next = (m_tail + 1) % kRing;
    if (next == m_head) return;         /* 満杯なら捨てる(起きない想定) */
    m_ring[m_tail] = { m_written, (uint32_t)(m_written + n), level };
    m_written += n;
    m_tail = next;
  }

  Entry m_ring[kRing];
  volatile size_t m_head = 0, m_tail = 0;   /* 書き手 1 / 読み手 1 */
  uint32_t m_written = 0;
  uint32_t m_t0 = 0;
  volatile bool m_playing = false;
};

SanoTTS tts;
LipSyncSpeaker spk;
Avatar avatar;

static const char* kLines[] = {
  "こんにちは。私はスタックチャンです。",
  "今日は良い天気ですね。",
  "電池の残量は八十五パーセントです。",
  "画面をタッチすると、次の文を喋ります。",
};
static const size_t kNumLines = sizeof(kLines) / sizeof(kLines[0]);
static const char* kKanaLine = "きょ][おわよ][いて][んきです°ね";

static QueueHandle_t g_queue;           /* const char* を積む */

static void ttsTask(void*) {
  const char* text;
  for (;;) {
    if (xQueueReceive(g_queue, &text, portMAX_DELAY) != pdTRUE) continue;
    avatar.setSpeechText(text);
    if (!tts.say(text)) Serial.println(tts.lastError());
    else Serial.printf("%s (%u samples, absmax %d)\n", text,
                       (unsigned)tts.samples(), (int)tts.absmax());
    avatar.setSpeechText("");
  }
}

static void say(const char* text) {
  xQueueSend(g_queue, &text, 0);        /* 喋っている最中なら後回し(最大 4 件) */
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  delay(300);

  tts.setSpeaker(&spk);
  if (!tts.begin()) { Serial.println(tts.lastError()); return; }
  Serial.printf("kanji: %s\n", tts.kanjiReady() ? "ready" : "NOT ready (dict missing)");

  avatar.setSpeechFont(&fonts::efontJA_16);   /* 吹き出しを日本語で */
  avatar.init();                              /* 描画タスクが core 1 で走り始める */

  g_queue = xQueueCreate(4, sizeof(const char*));
  xTaskCreatePinnedToCore(ttsTask, "tts", 16384, nullptr, 2, nullptr, PRO_CPU_NUM);

  say(tts.kanjiReady() ? kLines[0] : kKanaLine);
}

void loop() {
  M5.update();

  static size_t idx = 1;
  if (M5.Touch.getDetail().wasPressed()) {
    say(tts.kanjiReady() ? kLines[idx] : kKanaLine);
    idx = (idx + 1) % kNumLines;
  }

  /* 93 ms 刻みの値をそのまま渡すとカクつくので軽く追従させる */
  static float mouth = 0.0f;
  mouth += (spk.mouthOpenRatio() - mouth) * 0.5f;
  avatar.setMouthOpenRatio(mouth);

  delay(33);
}
