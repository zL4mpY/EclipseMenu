#include "recorder.hpp"

#include <memory>
#include <thread>
#include <utility>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/loader/Log.hpp>
#include <Geode/utils/general.hpp>
#include <modules/debug/benchmark.hpp>
#include <modules/recorder/DSPRecorder.hpp>
#include <modules/utils/SingletonCache.hpp>
#include <utils.hpp>

namespace eclipse::recorder {
    namespace ffmpeg = ffmpeg::events;

    class ProjectionDelegate : public cocos2d::CCDirectorDelegate {
        void updateProjection() override {
            kmGLMatrixMode(KM_GL_PROJECTION);
            kmGLLoadIdentity();
            kmMat4 orthoMatrix;
            auto size = utils::get<cocos2d::CCDirector>()->m_obWinSizeInPoints;
            kmMat4OrthographicProjection(&orthoMatrix, 0, size.width, size.height, 0, -1024, 1024);
            kmGLMultMatrix(&orthoMatrix);
            kmGLMatrixMode(KM_GL_MODELVIEW);
            kmGLLoadIdentity();
        }
    };

    void Recorder::start() {
        m_currentFrame.resize(m_renderSettings.m_width * m_renderSettings.m_height * 4, 0);
        m_renderTexture = RenderTexture(m_renderSettings.m_width, m_renderSettings.m_height);
        m_renderTexture.begin();

        m_recording = true;

        DSPRecorder::get()->start();

        utils::get<cocos2d::CCDirector>()->m_pProjectionDelegate = new ProjectionDelegate();
        std::thread(&Recorder::recordThread, this).detach();
    }

    void Recorder::stop() {
        if (!m_recording) return;

        m_recording = false;

        // make sure to let the recording thread know that we're stopping
        m_frameReady.set(true);

        m_renderTexture.end();
        DSPRecorder::get()->stop();

        auto director = utils::get<cocos2d::CCDirector>();
        if (auto& delegate = utils::get<cocos2d::CCDirector>()->m_pProjectionDelegate) {
            delete delegate;
            delegate = nullptr;
        }

        if (m_audioFifo) {
            fclose(m_audioFifo);
            m_audioFifo = nullptr;
        }

        director->setProjection(cocos2d::ccDirectorProjection::kCCDirectorProjection2D);
    }

    void Recorder::captureFrame() {
        // wait until the previous frame is processed
        m_frameReady.wait_for(false);

        // don't capture if we're not recording
        if (!m_recording) return;

        m_renderTexture.capture(utils::get<PlayLayer>(), m_currentFrame, m_frameReady);
    }

    std::string Recorder::getRecordingDuration() const {
        // m_recordingDuration is in nanoseconds
        double inSeconds = m_recordingDuration / 1'000'000'000.0;
        return utils::formatTime(inSeconds);
    }

    void Recorder::recordThread() {
        geode::utils::thread::setName("Eclipse Recorder Thread");
        geode::log::debug("Recorder thread started.");
        
        // Путь к FIFO в WINE (Z: = корень Linux)
        FILE* fifo = fopen("Z:\\tmp\\gd_vrecorder", "wb");
        if (!fifo) {
            m_callback("Failed to open FIFO. Run: mkfifo /tmp/gd_vrecorder");
            stop();
            return;
        }

        m_audioFifo = fopen("Z:\\tmp\\gd_arecorder", "wb");
        if (!m_audioFifo) {
            m_callback("Failed to open audio FIFO. Run: mkfifo /tmp/gd_arecorder");
            stop();
            return;
        }
    
        m_frameReady.set(false);
        m_frameReady.wait_for(true);
    
        debug::Timer timer("Recording", &m_recordingDuration);
    
        constexpr size_t FLOATS_PER_FRAME = (48000 / 60) * 2; // 800 * 2 = 1600 float'ов

        while (m_recording) {
            // Получаем аудио, соответствующее одному кадру
            auto audioFrame = DSPRecorder::get()->getLatestBuffer(FLOATS_PER_FRAME);

            // Пишем видео
            fwrite(m_currentFrame.data(), 1, m_currentFrame.size(), fifo);
            fflush(fifo);

            // Пишем аудио, если есть
            if (m_audioFifo && !audioFrame.empty()) {
                fwrite(audioFrame.data(), sizeof(float), audioFrame.size(), m_audioFifo);
                fflush(m_audioFifo);
            } else if (m_audioFifo) {
                // Если не хватает аудио — запишем тишину (чтобы не рассинхронизироваться)
                std::vector<float> silence(FLOATS_PER_FRAME, 0.0f);
                fwrite(silence.data(), sizeof(float), silence.size(), m_audioFifo);
                fflush(m_audioFifo);
            }

            if (!m_recording) break;

            m_frameReady.set(false);
            m_frameReady.wait_for(true);
        }
    
        fclose(fifo);
        geode::log::debug("Recorder thread stopped.");
        m_callback("Recording sent to FIFO. Check /tmp/gd_recorder.");
    }

    std::vector<std::string> Recorder::getAvailableCodecs() {
        return ffmpeg::Recorder::getAvailableCodecs();
    }
}
