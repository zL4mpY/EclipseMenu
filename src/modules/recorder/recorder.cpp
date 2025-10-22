#include "recorder.hpp"

#include <memory>
#include <thread>
#include <utility>
#include <fcntl.h>
#include <unistd.h> // для write, close
#include <errno.h>
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

        constexpr int TARGET_FPS = 60;
        constexpr int AUDIO_SAMPLE_RATE = 48000;
        constexpr int CHANNELS = 2; // stereo
        constexpr size_t SAMPLES_PER_FRAME = AUDIO_SAMPLE_RATE / TARGET_FPS; // 800
        constexpr size_t FLOATS_PER_FRAME = SAMPLES_PER_FRAME * CHANNELS;   // 1600

        int videoFd = open("/tmp/gd_vrecorder", O_WRONLY | O_NONBLOCK);
        if (videoFd == -1) {
            m_callback("Failed to open video FIFO (non-blocking)");
            stop();
            return;
        }

        int audioFd = open("/tmp/gd_arecorder", O_WRONLY | O_NONBLOCK);
        if (audioFd == -1) {
            m_callback("Failed to open audio FIFO (non-blocking)");
            close(videoFd);
            stop();
            return;
        }
    
        m_frameReady.set(false);
        m_frameReady.wait_for(true);
    
        debug::Timer timer("Recording", &m_recordingDuration);

        // while (m_recording) {
        //     // Получаем аудио, соответствующее одному кадру
        //     auto audioFrame = DSPRecorder::get()->getLatestBuffer(FLOATS_PER_FRAME);

        //     // Пишем видео
        //     fwrite(m_currentFrame.data(), 1, m_currentFrame.size(), fifo);
        //     fflush(fifo);

        //     // Пишем аудио, если есть
        //     if (m_audioFifo && !audioFrame.empty()) {
        //         fwrite(audioFrame.data(), sizeof(float), audioFrame.size(), m_audioFifo);
        //         fflush(m_audioFifo);
        //     } else if (m_audioFifo) {
        //         // Если не хватает аудио — запишем тишину (чтобы не рассинхронизироваться)
        //         std::vector<float> silence(FLOATS_PER_FRAME, 0.0f);
        //         fwrite(silence.data(), sizeof(float), silence.size(), m_audioFifo);
        //         fflush(m_audioFifo);
        //     }

        //     if (!m_recording) break;

        //     m_frameReady.set(false);
        //     m_frameReady.wait_for(true);
        // }
    
        while (m_recording) {
            // Видео
            ssize_t written = write(videoFd, m_currentFrame.data(), m_currentFrame.size());
            if (written == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // FIFO full or no reader — skip frame or log
                    geode::log::warn("Video FIFO not ready (no reader?)");
                } else {
                    geode::log::error("Video write error: {}", strerror(errno));
                    break;
                }
            }

            // Аудио
            std::vector<float> audioFrame = DSPRecorder::get()->getLatestBuffer(FLOATS_PER_FRAME);
            if (audioFrame.empty()) {
                audioFrame.assign(FLOATS_PER_FRAME, 0.0f);
            }

            written = write(audioFd, audioFrame.data(), audioFrame.size() * sizeof(float));
            if (written == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    geode::log::warn("Audio FIFO not ready");
                } else {
                    geode::log::error("Audio write error: {}", strerror(errno));
                    break;
                }
            }
        }

        close(videoFd);
        close(audioFd);
        geode::log::debug("Recorder thread stopped.");
        m_callback("Recording sent to FIFO. Check /tmp/gd_vrecorder.");
    }

    std::vector<std::string> Recorder::getAvailableCodecs() {
        return ffmpeg::Recorder::getAvailableCodecs();
    }
}
