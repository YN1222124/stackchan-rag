#pragma once
// ｽﾀｯｸﾁｬﾝの顔を描くモジュール
// 視線・表情・まばたき・呼吸・口パク・流れる文字を持ち、update() を loop から呼ぶだけで描画される。
// 描画は loop と同じスレッドで行うので、画面の取り合いが起きない。

#include <M5StackChan.h>

enum class Mood : uint8_t { Neutral = 0, Happy = 1, Sleepy = 2, Surprised = 3, Angry = 4 };

class Face {
public:
    void begin()
    {
        canvas_.setColorDepth(16);
        canvas_.setPsram(true);
        if (!canvas_.createSprite(M5.Display.width(), M5.Display.height())) {
            // PSRAMが使えない設定の場合は色数を落として内部RAMに確保する
            canvas_.setPsram(false);
            canvas_.setColorDepth(8);
            canvas_.createSprite(M5.Display.width(), M5.Display.height());
        }
        canvas_.setFont(&fonts::Font2);
        scheduleBlink(millis());
    }

    // 視線 x: -1(画面の左)〜1(画面の右), y: -1(下)〜1(上)
    void setLook(float x, float y)
    {
        lookTargetX_ = clampf(x, -1.0f, 1.0f);
        lookTargetY_ = clampf(y, -1.0f, 1.0f);
    }

    // ずっと続く表情
    void setMood(Mood m) { baseMood_ = m; }
    Mood baseMood() const { return baseMood_; }

    // 一時的な表情（ms 経過後に元へ戻る）
    void flashMood(Mood m, uint32_t ms)
    {
        tempMood_  = m;
        tempUntil_ = millis() + ms;
        hasTemp_   = true;
    }
    bool isFlashing() const { return hasTemp_; }

    // 口の開き具合 0〜1（負の値で口パクを止めて、表情の口に戻す）
    void setMouthOpen(float v) { mouthOpen_ = v < 0 ? -1.0f : clampf(v, 0.0f, 1.0f); }

    // 画面下に流れる1行の文字（空文字で非表示）。同じ文字なら流れる位置をリセットしない
    void setCaption(const String& s)
    {
        if (s == caption_) return;
        caption_  = s;
        capReset_ = true;        // 新しい文字は右端から流れ始める
        capLaps_  = 0;           // 流れきった回数もリセット
    }

    // 今の文字が流れきった回数（setCaption で文字を変えると0に戻る）
    uint32_t captionLaps() const { return capLaps_; }

    void update()
    {
        uint32_t now = millis();
        if (now - lastFrame_ < FRAME_MS) return;
        lastFrame_ = now;

        if (hasTemp_ && (int32_t)(now - tempUntil_) >= 0) hasTemp_ = false;
        Mood mood = hasTemp_ ? tempMood_ : baseMood_;

        // 首が止まっているときは、ときどき目だけをキョロッと動かす
        float tx = lookTargetX_, ty = lookTargetY_;
        if (fabsf(tx) < 0.05f && fabsf(ty) < 0.05f) {
            if ((int32_t)(now - nextSaccade_) >= 0) {
                saccX_       = random(-30, 31) / 100.0f;
                saccY_       = random(-20, 21) / 100.0f;
                nextSaccade_ = now + random(1500, 4000);
            }
            tx = saccX_;
            ty = saccY_;
        }
        lookX_ += (tx - lookX_) * 0.4f;
        lookY_ += (ty - lookY_) * 0.4f;

        // まばたき
        bool eyesOpen = true;
        if ((int32_t)(now - blinkStart_) >= 0) {
            if (now - blinkStart_ < BLINK_MS) {
                eyesOpen = false;
            } else {
                scheduleBlink(now);
            }
        }

        draw(mood, eyesOpen, now);
    }

private:
    static constexpr uint32_t FRAME_MS = 50;   // 約20fps
    static constexpr uint32_t BLINK_MS = 140;
    static constexpr int EYE_MOVE_X    = 24;   // 視線で目が動く最大量(px)
    static constexpr int EYE_MOVE_Y    = 14;

    static constexpr int CAPTION_SPEED = 3;    // 1フレームで動くpx（20fpsなので約60px/秒）
    static constexpr int CAPTION_GAP   = 80;   // 文字と文字の間隔(px)

    M5Canvas canvas_{&M5.Display};
    float lookX_ = 0, lookY_ = 0, lookTargetX_ = 0, lookTargetY_ = 0;
    float saccX_ = 0, saccY_ = 0;
    uint32_t nextSaccade_ = 0, blinkStart_ = 0, lastFrame_ = 0, tempUntil_ = 0;
    Mood baseMood_ = Mood::Neutral, tempMood_ = Mood::Neutral;
    bool hasTemp_  = false;
    float mouthOpen_ = -1.0f;                  // 負なら口パク無効
    String caption_;
    int  capX_     = 0;
    bool capReset_ = true;
    uint32_t capLaps_ = 0;

    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    void scheduleBlink(uint32_t now) { blinkStart_ = now + random(2500, 6000); }

    void draw(Mood mood, bool eyesOpen, uint32_t now)
    {
        auto& c           = canvas_;
        const uint16_t fg = TFT_WHITE, bg = TFT_BLACK;
        c.fillScreen(bg);

        int breath = (int)(sinf(now / 600.0f) * 2.0f);  // ゆっくり上下する呼吸
        int dx     = (int)(lookX_ * EYE_MOVE_X);
        int dy     = (int)(-lookY_ * EYE_MOVE_Y) + breath;

        const int cx     = c.width() / 2;
        const int eyeY   = 93 + dy;
        const int eyeXs[2] = {90 + dx, 230 + dx};
        const int mouthX = cx + dx / 2;             // 口は目より控えめに動かすと立体感が出る
        const int mouthY = 150 + dy / 2;
        const int r      = 9;

        // 目
        for (int ex : eyeXs) {
            switch (mood) {
                case Mood::Happy:  // ^ ^
                    c.fillArc(ex, eyeY + 5, r + 3, r - 1, 200, 340, fg);
                    break;
                case Mood::Sleepy:  // - -
                    c.fillRect(ex - r - 2, eyeY, (r + 2) * 2, 3, fg);
                    break;
                case Mood::Surprised:
                    if (eyesOpen) c.fillCircle(ex, eyeY, r + 4, fg);
                    else          c.fillRect(ex - r - 4, eyeY - 1, (r + 4) * 2, 3, fg);
                    break;
                default:
                    if (eyesOpen) c.fillCircle(ex, eyeY, r, fg);
                    else          c.fillRect(ex - r, eyeY - 1, r * 2, 3, fg);
                    break;
            }
        }

        // 怒りの眉（つり上がった線）
        if (mood == Mood::Angry) {
            c.drawWideLine(eyeXs[0] - 22, eyeY - 30, eyeXs[0] + 16, eyeY - 14, 4, fg);
            c.drawWideLine(eyeXs[1] + 22, eyeY - 30, eyeXs[1] - 16, eyeY - 14, 4, fg);
        }

        // 口
        if (mouthOpen_ >= 0 && mood != Mood::Sleepy) {
            // 口パク：開き具合 0〜1 で縦に伸びる楕円
            int h = 3 + (int)(mouthOpen_ * 22);
            c.fillEllipse(mouthX, mouthY, 22, h, fg);
        } else {
            switch (mood) {
                case Mood::Happy:
                    c.fillArc(mouthX, mouthY - 14, 26, 22, 30, 150, fg);
                    break;
                case Mood::Surprised:
                    c.fillEllipse(mouthX, mouthY, 9, 11, fg);
                    c.fillEllipse(mouthX, mouthY, 5, 7, bg);
                    break;
                case Mood::Sleepy:
                    c.fillRect(mouthX - 15, mouthY - 1, 30, 3, fg);
                    break;
                case Mood::Angry:
                    c.fillRect(mouthX - 22, mouthY, 44, 5, fg);
                    break;
                default:
                    c.fillRect(mouthX - 30, mouthY - 2, 60, 4, fg);
                    break;
            }
        }

        if (mood == Mood::Sleepy) {
            c.setTextColor(fg);
            c.setTextDatum(middle_center);
            c.drawString("z", 270, 50 - (int)((now / 120) % 20));
        }

        // 画面下に流れる文字（1行の電光掲示板）
        if (caption_.length() > 0) {
            c.setFont(&fonts::lgfxJapanGothic_16);       // 日本語フォント
            c.setTextColor(TFT_CYAN);
            c.setTextDatum(bottom_left);
            const int w      = c.textWidth(caption_.c_str());
            const int period = w + CAPTION_GAP;
            if (capReset_) { capX_ = c.width(); capReset_ = false; }
            capX_ -= CAPTION_SPEED;
            if (capX_ <= -period) { capX_ += period; capLaps_++; }   // 1周したら回数を数える
            for (int x = capX_; x < c.width(); x += period) {
                c.drawString(caption_.c_str(), x, c.height() - 6);
            }
            c.setFont(&fonts::Font2);                    // 元に戻す（"z" 用）
        }

        c.pushSprite(0, 0);
    }
};