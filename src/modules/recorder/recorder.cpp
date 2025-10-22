#include "recorder.hpp"

#include <memory>
#include <thread>
#include <fstream>
#include <nlohmann/json.hpp>
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

        director->setProjection(cocos2d::ccDirectorProjection::kCCDirectorProjection2D);
    }

    void Recorder::writeSettingsFile() {
        nlohmann::json settings;
        settings["width"] = m_renderSettings.m_width;
        settings["height"] = m_renderSettings.m_height;
        settings["fps"] = m_renderSettings.m_fps;
        settings["pixel_format"] = m_renderSettings.m_pixelFormat;
        settings["output"] = m_renderSettings.m_outputFile;
        settings["codec"] = m_renderSettings.m_codec;
        settings["bitrate"] = m_renderSettings.m_bitrate;
        settings["colorspace_filters"] = m_renderSettings.m_colorspaceFilters;


        std::ofstream file("Z:\\tmp\\gd_recorder_settings.json");
        if (file.is_open()) {
            file << settings.dump(2);
            file.close();
        }
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

        this->writeSettingsFile();

        FILE* fifo = fopen("Z:\\tmp\\gd_vrecorder", "wb");
        if (!fifo) {
            m_callback("Failed to open FIFO. Run: mkfifo /tmp/gd_vrecorder");
            stop();
            return;
        }
    
        m_frameReady.set(false);
        m_frameReady.wait_for(true);
    
        debug::Timer timer("Recording", &m_recordingDuration);

        while (m_recording) {
            fwrite(m_currentFrame.data(), 1, m_currentFrame.size(), fifo);
            fflush(fifo);

            if (!m_recording) break;

            m_frameReady.set(false);
            m_frameReady.wait_for(true);
        }
    
        fclose(fifo);
        DSPRecorder::get()->stop();

        FILE* done = fopen("Z:\\tmp\\gd_recording_done", "w");
        if (done) fclose(done);

        auto audioData = DSPRecorder::get()->getData();

        std::string audioTempPath = "Z:\\tmp\\gd_audio.f32";
        FILE* audioFile = fopen(audioTempPath.c_str(), "wb");
        if (audioFile) {
            fwrite(audioData.data(), sizeof(float), audioData.size(), audioFile);
            fclose(audioFile);
            geode::log::debug("Audio saved to {}", audioTempPath);
        } else {
            geode::log::error("Failed to save audio to {}", audioTempPath);
            m_callback("Failed to save audio.");
            return;
        }

        geode::log::debug("Recorder thread stopped.");
        m_callback("Recording sent to FIFO. Check /tmp/gd_vrecorder.");
    }

    std::vector<std::string> Recorder::getAvailableCodecs() {
        return ffmpeg::Recorder::getAvailableCodecs();
    }
}
