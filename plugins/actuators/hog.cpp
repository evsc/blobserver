#include "hog.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

using namespace std;
using namespace chrono;

/*****************/
// Class for parallel detection
/*****************/
class Parallel_Detect : public cv::ParallelLoopBody
{
    public:
        Parallel_Detect(const vector<cv::Point>* points, vector<cv::Point>* samples, const float margin,
            const Descriptor_Hog* descriptor, const CvSVM* svm, const cv::PCA* pca):
            _points(points), _samples(samples), _margin(margin), _descriptor(descriptor), _svm(svm), _pca(pca)
        {
            mMutex.reset(new mutex());
        }

        void operator()(const cv::Range& r) const
        {
            vector<float> description;
            cv::Mat descriptionMat;
            for (int idx = r.start; idx < r.end; ++idx)
            {
                const cv::Point point = (*_points)[idx];
                description = _descriptor->getDescriptor(point);
                if (description.size() == 0)
                    continue;
                descriptionMat = cv::Mat(1, description.size(), CV_32FC1, &description[0]);

                if (_pca != NULL)
                    descriptionMat = _pca->project(descriptionMat);
                
                descriptionMat = descriptionMat.t();

                if (_margin > 0.f)
                {
                    float distance;
                    try
                    {
                        distance = _svm->predict(descriptionMat, true);
                    }
                    catch (cv::Exception)
                    {
                        g_log(NULL, G_LOG_LEVEL_ERROR, "%s - An exception happened during a call to CvSVM::predict. Is the model file correct?", Actuator_Hog::getClassName().c_str());
                    }
                    if (distance < -_margin)
                    {
                        lock_guard<mutex> lock(*mMutex.get());
                        _samples->push_back(point);
                    }
                }
                else
                {
                    float distance;
                    try
                    {
                        g_log(NULL, G_LOG_LEVEL_ERROR, "%s - An exception happened during a call to CvSVM::predict. Is the model file correct?", Actuator_Hog::getClassName().c_str());
                    }
                    catch (cv::Exception)
                    {
                        g_log(NULL, G_LOG_LEVEL_WARNING, "error!");
                    }
                    if (distance == 1.f)
                    {
                        lock_guard<mutex> lock(*mMutex.get());
                        _samples->push_back(point);
                    }
                }
            }
        }

    private:
        const vector<cv::Point>* _points;
        vector<cv::Point>* _samples;
        const Descriptor_Hog* _descriptor;
        const CvSVM* _svm;
        const cv::PCA* _pca;
        const float _margin;

        shared_ptr<mutex> mMutex;
};

/*************/
// Definition of class Actuator_Hog
/*************/
std::string Actuator_Hog::mClassName = "Actuator_Hog";
std::string Actuator_Hog::mDocumentation = "N/A";
unsigned int Actuator_Hog::mSourceNbr = 1;

/*************/
Actuator_Hog::Actuator_Hog()
{
    make();
}

/*************/
Actuator_Hog::Actuator_Hog(int pParam)
{
    make();
}

/*************/
void Actuator_Hog::make()
{
    mOutputBuffer = cv::Mat::zeros(480, 640, CV_8UC3);

    mName = mClassName;
    mOscPath = "hog";

    mFilterSize = 3;
    mFilterDilateCoeff = 3;

    mBlobLifetime = 30;
    mKeepOldBlobs = 0;
    mKeepMaxTime = 0;
    mProcessNoiseCov = 1e-6;
    mMeasurementNoiseCov = 1e-4;

    mRoiSize = cv::Size_<int>(64, 128);
    mBlockSize = cv::Size_<int>(2, 2);
    mCellSize = cv::Size_<int>(16, 16);
    mCellMaxSize = cv::Size_<int>(0, 0);
    mCellStep = cv::Size_<float>(2.f, 2.f);
    mBins = 9;
    mSigma = 0.f;
    updateDescriptorParams();

    mIsPcaLoaded = false;

    mSvmMargin = 0.f;
    mIsModelLoaded = false;
    mMaxTimePerFrame = 1e5;
    mMaxThreads = 4;

    mBlobMergeDistance = 64.f;
    mSaveSamples = false;
    mSaveSamplesAge = 120;

    movement = 0;
}

/*************/
atom::Message Actuator_Hog::detect(const vector< Capture_Ptr > pCaptures)
{
    vector<cv::Mat> captures = captureToMat(pCaptures);
    if (captures.size() < mSourceNbr)
    {
        g_log(NULL, G_LOG_LEVEL_WARNING, "%s: Not enough valid sources to process", mClassName.c_str());
        return mLastMessage;
    }

    unsigned long long timeStart = duration_cast<microseconds>(high_resolution_clock::now().time_since_epoch()).count();

    if (captures.size() == 0 || !mIsModelLoaded)
        return mLastMessage;

    // For simplicity...
    cv::Mat input = captures[0].clone();

    // We get windows of interest, using BG subtraction
    // and previous blobs positions
    mBgSubtractor(input, mBgSubtractorBuffer);
    // Erode and dilate to suppress noise
    cv::Mat lEroded;
    cv::threshold(mBgSubtractorBuffer, mBgSubtractorBuffer, 250, 255, cv::THRESH_BINARY);
    cv::erode(mBgSubtractorBuffer, lEroded, cv::Mat(), cv::Point(-1, -1), mFilterSize);

    mBgSubtractorBuffer = cv::Mat::zeros(mBgSubtractorBuffer.size(), CV_8U);
    movement = 0;
    for (uint x = 0; x < lEroded.cols; ++x)
        for (uint y = 0; y < lEroded.rows; ++y)
        {
            if (lEroded.at<uchar>(y, x) > 0)
            {
                cv::Rect rect(x - mRoiSize.width / 2, y - mRoiSize.height / 2, mRoiSize.width, mRoiSize.height);
                cv::rectangle(mBgSubtractorBuffer, rect, 255, CV_FILLED);
                movement = 1;
            }
        }

    // We draw rectangles to handle previously detected blobs
    for_each (mBlobs.begin(), mBlobs.end(), [&] (Blob2D blob)
    {
        Blob::properties props = blob.getBlob();
        cv::Rect rect(props.position.x - props.size/2, props.position.y - props.size/2, props.size, props.size);
        cv::rectangle(mBgSubtractorBuffer, rect, 255, CV_FILLED);
    } );

    // The result is resized according to cell size
    cv::Size outputSize;
    outputSize.width = mBgSubtractorBuffer.cols / mCellSize.width;
    outputSize.height = mBgSubtractorBuffer.rows / mCellSize.height;
    cv::Mat resizedBuffer;
    cv::resize(mBgSubtractorBuffer, resizedBuffer, outputSize, 0, 0, cv::INTER_NEAREST);

    // We feed the image to the descriptor
    mDescriptor.setImage(input);

    // We fill the vector of all positions to test
    if (mSvmValidPositions.capacity() != outputSize.width * outputSize.height)
        mSvmValidPositions.reserve(outputSize.width * outputSize.height);

    int validPositions = 0;
    for (int x = 0; x < resizedBuffer.cols; ++x)
        for (int y = 0; y < resizedBuffer.rows; ++y)
        {
            if (resizedBuffer.at<uchar>(y, x) < 255)
                continue;

            vector<cv::Point>::iterator it = mSvmValidPositions.begin() + validPositions;
            *it = cv::Point(x, y);
            validPositions++;
        }
    int totalSamples = validPositions;

    // We go randomly through this list
    vector<cv::Point> samples;
    vector<float> description;
    cv::Mat descriptionMat;

    unsigned long long timePresent = duration_cast<microseconds>(high_resolution_clock::now().time_since_epoch()).count();
    while (validPositions && timePresent - timeStart < mMaxTimePerFrame)
    {
        vector<cv::Point> points;
        int nbrPoints = min(mMaxThreads, validPositions);
        for (int i = 0; i < nbrPoints; ++i)
        {
            unsigned int random = mRng();
            unsigned int position = random % validPositions;
            vector<cv::Point>::iterator it = mSvmValidPositions.begin() + position;
            cv::Point point = *it;
            vector<cv::Point>::iterator lastIt = mSvmValidPositions.begin() + validPositions - 1;
            swap(*lastIt, *it);

            point.x *= mCellSize.width;
            point.y *= mCellSize.height;

            validPositions--;
            points.push_back(point);
        }

        if (mIsPcaLoaded)
            cv::parallel_for_(cv::Range(0, nbrPoints), Parallel_Detect(&points, &samples, mSvmMargin, &mDescriptor, &mSvm, &mPca));
        else
            cv::parallel_for_(cv::Range(0, nbrPoints), Parallel_Detect(&points, &samples, mSvmMargin, &mDescriptor, &mSvm, NULL));

        timePresent = duration_cast<microseconds>(high_resolution_clock::now().time_since_epoch()).count();
    }

    // A single object can be detected by multiple windows.
    // We need to merge them
    for (int i = 0; i < samples.size(); ++i)
    {
        float meanFactor = 1.f;
        for (int j = i + 1; j < samples.size();)
        {
            float distance = sqrtf(pow(samples[i].x - samples[j].x, 2.f) + pow(samples[i].y - samples[j].y, 2.f));
            if (distance < mBlobMergeDistance)
            {
                meanFactor++;
                samples[i].x = (int)((float)samples[i].x * (meanFactor - 1.f)/meanFactor + (float)samples[j].x * 1.f / meanFactor);
                samples[i].y = (int)((float)samples[i].y * (meanFactor - 1.f)/meanFactor + (float)samples[j].y * 1.f / meanFactor);

                vector<cv::Point>::iterator it = samples.begin() + j;
                samples.erase(it);
            }
            else
                j++;
        }
    }

    // We create the properties which will be converted to blobs
    vector<Blob::properties> properties;
    for (int i = 0; i < samples.size(); ++i)
    {
        Blob::properties propertie;
        propertie.position.x = samples[i].x;
        propertie.position.y = samples[i].y;
        propertie.size = mRoiSize.width;
        propertie.speed.x = 0.f;
        propertie.speed.y = 0.f;

        properties.push_back(propertie);
    }

    // We want to track them
    trackBlobs<Blob2D>(properties, mBlobs, mBlobLifetime, mKeepOldBlobs, mKeepMaxTime);

    // We make sure that the filtering parameters are set
    for (int i = 0; i < mBlobs.size(); ++i)
    {
        mBlobs[i].setParameter("processNoiseCov", mProcessNoiseCov);
        mBlobs[i].setParameter("measurementNoiseCov", mMeasurementNoiseCov);
    }

    // We delete blobs which are outside the frame
    for (int i = 0; i < mBlobs.size();)
    {
        Blob::properties prop = mBlobs[i].getBlob();
        if (prop.position.x + prop.size/2 > input.cols || prop.position.x + prop.size/2 < 0
            || prop.position.y + prop.size/2 > input.rows || prop.position.y + prop.size/2 < 0)
            mBlobs.erase(mBlobs.begin() + i);
        else
            i++;
    }

    cv::Mat resultMat = cv::Mat::zeros(input.rows, input.cols, input.type());
    for_each (mBlobs.begin(), mBlobs.end(), [&] (Blob2D blob)
    {
        Blob::properties props = blob.getBlob();
        cv::Rect rect(props.position.x, props.position.y, mRoiSize.width, mRoiSize.height);
        cv::rectangle(resultMat, rect, cv::Scalar(1, 1, 1), CV_FILLED);

        if (mSaveSamples && blob.getAge() == mSaveSamplesAge
            && rect.x >= 0 && rect.y >= 0 && input.cols - rect.width > rect.x && input.rows - rect.height > rect.y)
        {
            cv::Mat cropSample(input, rect);
            char buffer[64];
            sprintf(buffer, "sample_%i.png", blob.getId());
            cv::imwrite(buffer, cropSample);
        }
    } );

    // The result is shown
    cv::multiply(input, resultMat, resultMat);

    if (mVerbose && totalSamples != 0)
        g_log(NULL, G_LOG_LEVEL_DEBUG, "%s - Evaluated ratio = %f", mClassName.c_str(), 1.f - (float)validPositions / (float)totalSamples);

    // Constructing the message
    mLastMessage.clear();
    mLastMessage.push_back(atom::IntValue::create((int)mBlobs.size()));
    mLastMessage.push_back(atom::IntValue::create(7));

    for(int i = 0; i < mBlobs.size(); ++i)
    {
        int lX, lY, lSize, lId, lAge, lLost;
        float ldX, ldY;
        Blob::properties properties = mBlobs[i].getBlob();
        lX = (int)(properties.position.x);
        lY = (int)(properties.position.y);
        ldX = properties.speed.x;
        ldY = properties.speed.y;
        lId = (int)mBlobs[i].getId();
        lAge = (int)mBlobs[i].getAge();
        lLost = (int)mBlobs[i].getLostDuration();

        // Print the blob number on the blob
        if (mVerbose)
        {
            char lNbrStr[8];
            cv::Scalar fontcolor;
            if (lLost <= 0) fontcolor = cv::Scalar(255.0, 0.0, 0.0, 128.0);
            else fontcolor = cv::Scalar(128.0, 128.0, 128.0, 128.0);
            sprintf(lNbrStr, "%i", lId);
            cv::putText(resultMat, lNbrStr, cv::Point(lX, lY), cv::FONT_HERSHEY_COMPLEX, 0.66, fontcolor);
        }

        // Add this blob to the message
        mLastMessage.push_back(atom::IntValue::create(lId));
        mLastMessage.push_back(atom::IntValue::create(lX));
        mLastMessage.push_back(atom::IntValue::create(lY));
        mLastMessage.push_back(atom::FloatValue::create(ldX));
        mLastMessage.push_back(atom::FloatValue::create(ldY));
        mLastMessage.push_back(atom::IntValue::create(lAge));
        mLastMessage.push_back(atom::IntValue::create(lLost));

        // add movement info
        mLastMessage.push_back(atom::IntValue::create(movement));
    }

    mOutputBuffer = resultMat.clone();

    return mLastMessage;
}

/*************/
void Actuator_Hog::setParameter(atom::Message pMessage)
{
    std::string cmd;
    try
    {
        cmd = toString(pMessage[0]);
    }
    catch (atom::BadTypeTagError error)
    {
        return;
    }

    if (cmd == "modelFilename")
    {
        string filename;
        if (!readParam(pMessage, filename))
            return;

        g_log(NULL, G_LOG_LEVEL_INFO, "%s - Attempting to load SVM model from file %s", mClassName.c_str(), filename.c_str());
        mSvm.load(filename.c_str());
        mIsModelLoaded = true;
    }
    else if (cmd == "pcaFilename")
    {
        string filename;
        if (!readParam(pMessage, filename))
            return;

        g_log(NULL, G_LOG_LEVEL_INFO, "%s - Attempting to load PCA transform from file from file %s", mClassName.c_str(), filename.c_str());
        cv::FileStorage file(filename, cv::FileStorage::READ);
        cv::Mat eigen, mean;
        file["eigenVectors"] >> eigen;
        file["mean"] >> mean;
        mPca.eigenvectors = eigen;
        mPca.mean = mean;
        mIsPcaLoaded = true;
    }
    else if (cmd == "maxTimePerFrame")
    {
        float duration;
        if (readParam(pMessage, duration))
            mMaxTimePerFrame = max(33000, (int)duration);
    }
    else if (cmd == "maxThreads")
    {
        float nbr;
        if (readParam(pMessage, nbr))
            mMaxThreads = max(1, (int)nbr);
    }
    else if (cmd == "mergeDistance")
    {
        float distance;
        if (readParam(pMessage, distance))
            mBlobMergeDistance = max(16.f, distance);
    }
    else if (cmd == "filterSize")
    {
        float filterSize;
        if (readParam(pMessage, filterSize))
            mFilterSize = max(1, (int)filterSize);
    }
    else if (cmd == "roiSize")
    {
        string roiStr;
        if (!readParam(pMessage, roiStr))
            return;

        cv::Size_<int> roiSize;
        sscanf(roiStr.c_str(), "size_%ix%i", &(roiSize.width), &(roiSize.height));
        if (roiSize.width != 0 && roiSize.height != 0)
            mRoiSize = roiSize;
        updateDescriptorParams();
    }
    else if (cmd == "blockSize")
    {
        string blockStr;
        if (!readParam(pMessage, blockStr))
            return;

        cv::Size_<int> blockSize;
        sscanf(blockStr.c_str(), "size_%ix%i", &(blockSize.width), &(blockSize.height));
        if (blockSize.width != 0 && blockSize.height != 0)
            mBlockSize = blockSize;
        updateDescriptorParams();
    }
    else if (cmd == "cellSize")
    {
        string cellStr;
        if (!readParam<string>(pMessage, cellStr))
            return;

        cv::Size_<int> cellSize;
        sscanf(cellStr.c_str(), "size_%ix%i", &(cellSize.width), &(cellSize.height));
        if (cellSize.width != 0 && cellSize.height != 0)
            mCellSize = cellSize;
        updateDescriptorParams();
    }
    else if (cmd == "cellMaxSize")
    {
        string cellStr;
        if (!readParam<string>(pMessage, cellStr))
            return;

        cv::Size_<int> cellSize;
        sscanf(cellStr.c_str(), "size_%ix%i", &(cellSize.width), &(cellSize.height));
        if (cellSize.width > 0 && cellSize.height > 0)
            mCellMaxSize = cellSize;
        updateDescriptorParams();
    }
    else if (cmd == "cellStep")
    {
        string step;
        if (!readParam<string>(pMessage, step))
            return;

        cv::Size_<float> cellStep;
        sscanf(step.c_str(), "size_%fx%f", &(cellStep.width), &(cellStep.height));
        if (cellStep.width >= 1.f && cellStep.height >= 1.f)
            mCellStep = cellStep;
        updateDescriptorParams();
    }
    else if (cmd == "bins")
    {
        float bins;
        if (!readParam<float>(pMessage, bins))
            return;

        mBins = max(2.f, bins);
        updateDescriptorParams();
    }
    else if (cmd == "margin")
    {
        float margin;
        if (!readParam<float>(pMessage, margin))
            return;

        mSvmMargin = max(0.f, margin);
    }
    else if (cmd == "lifetime")
    {
        float lifetime;
        if (readParam(pMessage, lifetime))
            mBlobLifetime = lifetime;
    }
    else if (cmd == "keepOldBlobs")
    {
        float keep;
        if (readParam(pMessage, keep, 1))
            mKeepOldBlobs = (int)keep;
        if (readParam(pMessage, keep, 2))
            mKeepMaxTime = (int)keep;
    }
    else if (cmd == "processNoiseCov")
    {
        float cov;
        if (readParam(pMessage, cov))
            mProcessNoiseCov = abs(cov);
    }
    else if (cmd == "measurementNoiseCov")
    {
        float cov;
        if (readParam(pMessage, cov))
            mMeasurementNoiseCov = abs(cov);
    }
    else if (cmd == "saveSamples")
    {
        float save;
        if (!readParam(pMessage, save))
            return;

        if (save == 1.f)
            mSaveSamples = true;
        else
            mSaveSamples = false;
    }
    else if (cmd == "saveSamplesAge")
    {
        float age;
        if (readParam(pMessage, age))
            mSaveSamplesAge = (unsigned long)age;
    }
    else
        setBaseParameter(pMessage);
}

/*************/
void Actuator_Hog::updateDescriptorParams()
{
    mDescriptor.setHogParams(mRoiSize, mBlockSize, mCellSize, mBins, false, Descriptor_Hog::L2_NORM, mSigma);
    if (mCellMaxSize.width >= mCellSize.width && mCellMaxSize.height >= mCellSize.height && mCellStep.width >= 1.f && mCellStep.height >= 1.f)
        mDescriptor.setMultiscaleParams(mCellSize, mCellMaxSize, mCellStep);
}
