#pragma once

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

enum class ProgressOperation
{
    GENERIC,
    DOWNLOAD,
    EXTRACT,
    FORWARDER,
    INSTALL
};

class ProgressEvent
{
private:
    ProgressEvent() {}

    mutable std::mutex _mutex;
    int _current = 0;
    int _max = 60;
    double _now = 0;
    double _total = 0;
    double _speed = 0;
    long _status_code = 0;
    std::atomic_bool _interupt{false};

    ProgressOperation _operation = ProgressOperation::GENERIC;
    std::string _current_file;
    double _file_now = 0;
    double _file_total = 0;
    std::string _error_message;

public:
    ProgressEvent(const ProgressEvent&) = delete;
    ProgressEvent& operator=(const ProgressEvent&) = delete;
    ProgressEvent(ProgressEvent&&) = delete;
    ProgressEvent& operator=(ProgressEvent&&) = delete;

    static auto& instance()
    {
        static ProgressEvent event;
        return event;
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _current = 0;
        _max = 60;
        _now = 0;
        _total = 0;
        _speed = 0;
        _status_code = 0;
        _interupt.store(false, std::memory_order_relaxed);
        _operation = ProgressOperation::GENERIC;
        _current_file.clear();
        _file_now = 0;
        _file_total = 0;
        _error_message.clear();
    }

    inline void setTotalSteps(int steps)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _max = steps;
    }

    inline void setTotalCount(double total)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _total = total;
    }

    inline void setSpeed(double speed)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _speed = speed;
    }

    inline void setStep(int step)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _current = step;
    }

    inline void setStatusCode(long status_code)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _status_code = status_code;
    }

    inline void incrementStep(int increment)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _current += increment;
    }

    inline void setNow(double now)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _now = now;
    }

    inline void incrementNow(double increment)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _now += increment;
    }

    inline int getStep()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _current;
    }

    inline double getNow()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _now;
    }

    inline bool finished()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return (_current == _max);
    }

    inline int getMax()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _max;
    }

    inline double getTotal()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _total;
    }

    inline double getSpeed()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _speed;
    }

    inline double getStatusCode()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _status_code;
    }

    inline void setInterupt(bool interupt)
    {
        _interupt.store(interupt, std::memory_order_relaxed);
    }

    inline bool getInterupt() const
    {
        return _interupt.load(std::memory_order_relaxed);
    }

    
    
    inline void setDownloadProgress(double now, double total)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _now = now;
        _total = total;
        _file_now = now;
        _file_total = total;

        if (_max > 1 && total > 0.0) {
            const int step = static_cast<int>((now / total) * _max);
            _current = std::min(_max - 1, std::max(0, step));
        }
    }

    inline void setExtractionProgress(double now, double total, double file_now, double file_total)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _now = now;
        _total = total;
        _file_now = file_now;
        _file_total = file_total;

        if (_max > 1 && total > 0.0) {
            const int step = static_cast<int>((now / total) * _max);
            _current = std::min(_max - 1, std::max(0, step));
        }
    }

    inline void setInstallProgress(double now, double total, const std::string& message, int contentIndex, int contentCount)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _operation = ProgressOperation::INSTALL;
        _current_file = message;
        _now = now;
        _total = total;
        _file_now = contentIndex;
        _file_total = contentCount;
        if (_max < 100) _max = 100;
        if (total > 0.0)
            _current = std::clamp(static_cast<int>((now / total) * 99.0), 0, 99);
    }

    inline void setForwarderProgress(int current, int total, const std::string& message)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _operation = ProgressOperation::FORWARDER;
        _current_file = message;
        _now = current;
        _total = total;

        _max = std::max(1, total + 1);
        _current = std::clamp(current, 0, std::max(0, total));
    }

    inline void setErrorMessage(const std::string& message)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _error_message = message;
    }

    inline std::string getErrorMessage()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _error_message;
    }

    inline void setOperation(ProgressOperation operation)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _operation = operation;
    }

    inline ProgressOperation getOperation()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _operation;
    }

    inline void setCurrentFile(const std::string& current_file)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _current_file = current_file;
    }

    inline std::string getCurrentFile()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _current_file;
    }

    inline void setFileProgress(double now, double total)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _file_now = now;
        _file_total = total;
    }

    inline void incrementFileNow(double increment)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _file_now += increment;
    }

    inline double getFileNow()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _file_now;
    }

    inline double getFileTotal()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _file_total;
    }
};
