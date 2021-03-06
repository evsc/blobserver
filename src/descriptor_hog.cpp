#include "descriptor_hog.h"

using namespace std;

/*************/
// Class for parallel computation of gradients
class Parallel_Gradients : public cv::ParallelLoopBody
{
    public:
        Parallel_Gradients(cv::Mat* gradientsH, cv::Mat* gradientsV, cv::Mat* gradients, const int channels, const bool isSigned):
            _gradientsH(gradientsH), _gradientsV(gradientsV), _gradients(gradients), _cn(channels), _signed(isSigned) {}

        void operator()(const cv::Range& r) const
        {
            for (int y = r.start; y != r.end; ++y)
            {
                for (int x = 0; x < _gradients->cols; ++x)
                {
                    float hValue = 0.f;
                    float vValue = 0.f;
                    float length = 0.f;

                    for (int i = 0; i < _cn; ++i)
                    {
                        float cnHValue = _gradientsH[i].at<short>(y, x);
                        float cnVValue = _gradientsV[i].at<short>(y, x);
                        float cnLength = sqrtf(cnHValue*cnHValue + cnVValue*cnVValue);

                        if (cnLength > length)
                        {
                            hValue = cnHValue;
                            vValue = cnVValue;
                            length = cnLength;
                        }
                    }

                    float angle = 0.f;
                    if (length > 0.f)
                    {
                        hValue /= length;
                        angle = acos(hValue);
                    }

                    if (_signed && vValue < 0.f)
                    {
                        angle = 2.0*CV_PI - angle;
                        angle = (int)(angle / CV_PI * 180) % 360;
                    }
                    else
                    {
                        angle = (int)(angle / CV_PI * 180) % 180;
                    }

                    _gradients->at<cv::Vec2b>(y, x)[0] = (char)angle;
                    _gradients->at<cv::Vec2b>(y, x)[1] = (char)length;
                }
            }
        }

    private:
        cv::Mat* _gradientsH;
        cv::Mat* _gradientsV;
        cv::Mat* _gradients;
        const int _cn;
        const bool _signed;
};

/*************/
// Definition of class Descriptor_Hog
/*************/
Descriptor_Hog::Descriptor_Hog():
    _doCrop(false)
{
    _roiSize = cv::Size_<int>(64, 128);
    _blockSize = cv::Size_<int>(3, 3);
    _cellSize = cv::Size_<int>(8, 8);
    _binsPerCell = 9;
    _signed = false;
    _normType = L1_NORM;
    _gaussSigma = 0.0f;

    _epsilon = FLT_EPSILON;
    _kernelH = cv::Mat(1, 3, CV_32FC1);
    _kernelH.at<float>(0, 0) = -1.f;
    _kernelH.at<float>(0, 1) = 0.f;
    _kernelH.at<float>(0, 2) = 1.f;
    _kernelV = _kernelH.t();

    _cellMinSize = cv::Size_<int>(0, 0);
    _cellMaxSize = cv::Size_<int>(0, 0);
    _cellStep = cv::Size_<float>(1.f, 1.f);
}

/*************/
void Descriptor_Hog::setImage(const cv::Mat& pImage)
{
    cv::Mat tmpImage = pImage.clone();

    if (_doCrop)
        _image = cv::Mat(tmpImage, _cropRect);
    else
        _image = tmpImage;

    cv::Mat gradientsH; //(_image.rows, _image.cols, pImage.type());
    cv::Mat gradientsV; //(_image.rows, _image.cols, pImage.type());
    int cn = CV_MAT_CN(_image.type());

    cv::filter2D(_image, gradientsH, CV_16S, _kernelH);
    cv::filter2D(_image, gradientsV, CV_16S, _kernelV);

    // Compute the oriented gradient for each pixel
    cv::Mat channelsH[cn];
    cv::Mat channelsV[cn];
    cv::split(gradientsH, channelsH);
    cv::split(gradientsV, channelsV);

    if (_gradients.cols != _image.cols || _gradients.rows != _image.rows)
        _gradients = cv::Mat(_image.rows, _image.cols, CV_8UC2);

    cv::parallel_for_(cv::Range(0, _gradients.rows), Parallel_Gradients(channelsH, channelsV, &_gradients, cn, _signed));
}

/*************/
vector<float> Descriptor_Hog::getDescriptor(cv::Point_<int> pPos) const
{
    vector<float> descriptor;

    if (_cellMinSize == cv::Size_<int>(0, 0) ||
        _cellMaxSize == cv::Size_<int>(0, 0) ||
        _cellStep == cv::Size_<float>(0.f, 0.f))
    {
        descriptor = getSingleScaleDescriptor(pPos, _cellSize);
    }
    else
    {
        cv::Size_<int> cellSize = _cellMinSize;
        
        while (cellSize.width <= _cellMaxSize.width) // && cellSize.height <= _cellMaxSize.height)
        {
            cellSize.height = _cellMinSize.height;

            while (cellSize.height <= _cellMaxSize.height)
            {
                int currentSize = descriptor.size();
                vector<float> addedVectors = getSingleScaleDescriptor(pPos, cellSize);
                descriptor.resize(currentSize + addedVectors.size());
                copy(addedVectors.begin(), addedVectors.end(), descriptor.begin() + currentSize);

                cellSize.height = (int)((float)cellSize.height * _cellStep.height);
            }

            cellSize.width = (int)((float)cellSize.width * _cellStep.width);
        }
    }

    return descriptor;
}

/*************/
vector<float> Descriptor_Hog::getSingleScaleDescriptor(cv::Point_<int> pPos, const cv::Size_<int> pCellSize) const
{
    vector<float> descriptor;

    // Angle covered per bin
    float anglePerBin = 180.f / (float)_binsPerCell;
    if (_signed)
        anglePerBin *= 2.f;
    float binPerAngle = 1.f / anglePerBin;

    // Check if we have enough room to build a complete descriptor
    if (pPos.x + _roiSize.width > _image.cols || pPos.y + _roiSize.height > _image.rows)
        return descriptor;

    // If position is too close to the border (no margin), we don't have enough room either
    if (pPos.x < 0 || pPos.y < 0)
        return descriptor;

    int cellNumberH = _roiSize.width / pCellSize.width;
    int cellNumberV = _roiSize.height / pCellSize.height;
    
    // For each cell, we compute its descriptor
    vector< vector<float> > cellsDescriptor;
    for (int cellH = 0; cellH < cellNumberH; ++cellH)
        for (int cellV = 0; cellV < cellNumberV; ++cellV)
        {
            cv::Point_<int> topLeft;
            topLeft.x = cellH * pCellSize.width + pPos.x;
            topLeft.y = cellV * pCellSize.height + pPos.y;
            
            // Creation of the histogram
            vector<float> cellDescriptor;
            cellDescriptor.assign(_binsPerCell, 0);
            for (int x = 0; x < pCellSize.width; ++x)
                for (int y = 0; y < pCellSize.height; ++y)
                {
                    cv::Point position(topLeft.x + x, topLeft.y + y);
                    int index = _gradients.at<cv::Vec2b>(position.y, position.x)[0] * binPerAngle;
                    float subPos = _gradients.at<cv::Vec2b>(position.y, position.x)[0] - index*anglePerBin;
                    int shift = (subPos < anglePerBin*0.5f) ? -1 : 1;
                    if (shift + index < 0)
                        shift = _binsPerCell-1;
                    else if (shift + index >= _binsPerCell)
                        shift = 0;
                    else
                        shift += index;

                    float ratio = abs(subPos - anglePerBin*0.5f) * binPerAngle;
                    cellDescriptor[index] += _gradients.at<cv::Vec2b>(position.y, position.x)[1] * (1.f - ratio);
                    cellDescriptor[shift] += _gradients.at<cv::Vec2b>(position.y, position.x)[1] * ratio;
                }

            // We normalize the cell
            float invNorm = 1.f / getDescriptorNorm(cellDescriptor);
            for (int i = 0; i < _binsPerCell; ++i)
                cellDescriptor[i] *= invNorm;

            cellsDescriptor.push_back(cellDescriptor);
        }

    // We have all cells descriptors. Now we normalize them to create the global descriptor
    for (int cellH = 0; cellH < cellNumberH - (_blockSize.width - 1); ++cellH)
        for (int cellV = 0; cellV < cellNumberV - (_blockSize.height - 1); ++cellV)
        {
            int blockCenterH = cellH + (_blockSize.width << 2);
            int blockCenterV = cellV + (_blockSize.height << 2);

            vector<float> descriptorVector;
            descriptorVector.assign(_binsPerCell * (_blockSize.width*_blockSize.height), 0);
            // We calculate the norm of the descriptor over the whole block
            // So we go over all the cells for the current block
            for (int i = 0; i < _blockSize.width; ++i)
                for (int j = 0; j < _blockSize.height; ++j)
                {
                    //int index = blockCenterH+i + (blockCenterH+j)*windowV;
                    int index = cellH+i + (cellV+j) * cellNumberH;

                    // We apply a gaussian curve over the blocks
                    float gaussianFactor = 1.f;
                    if (_gaussSigma != 0.f)
                    {
                        float distToCenterBlock = sqrtf(pow((float)i - (float)_blockSize.width*0.5f + 0.5, 2.f) + pow((float)j - (float)_blockSize.height*0.5f + 0.5, 2.f));
                        gaussianFactor = getGaussian(distToCenterBlock, _gaussSigma);
                    }

                    for (int orientation = 0; orientation < _binsPerCell; ++orientation)
                        descriptorVector[orientation + (i + j*_blockSize.width)*_binsPerCell] = cellsDescriptor[index][orientation] * gaussianFactor;
                }

            // We need the norm for the current block descriptor
            float invNorm = 1.f / getDescriptorNorm(descriptorVector);

            // And we scale the whole block, to get the descriptor for the current cell
            for (int i = 0; i < _binsPerCell * (_blockSize.width*_blockSize.height); ++i)
                descriptor.push_back(descriptorVector[i] * invNorm);
        }

    return descriptor;
}

/*************/
void Descriptor_Hog::setRoi(cv::Rect_<int> pCropRect)
{
    if (pCropRect.height && pCropRect.width)
    {
        _doCrop = true;
        _cropRect = pCropRect;
    }
}

/*************/
void Descriptor_Hog::setHogParams(const cv::Size_<int> pDescriptorSize, const cv::Size_<int> pBlockSize, const cv::Size_<int> pCellSize,
    const unsigned int pBinsPerCell, const bool pSigned, const Hog_Norm pNorm, const float pSigma)
{
    if (pDescriptorSize == cv::Size_<int>(0, 0) ||
        pBlockSize == cv::Size_<int>(0, 0) ||
        pCellSize == cv::Size_<int>(0, 0))
    {
        return;
    }

    _blockSize = pBlockSize;
    _cellSize = pCellSize;
    _roiSize = pDescriptorSize;
    _binsPerCell = pBinsPerCell;
    _signed = pSigned;
    _normType = pNorm;

    _gaussSigma = max(0.f, pSigma);
}

/*************/
void Descriptor_Hog::setMultiscaleParams(const cv::Size_<int> pCellMinSize, const cv::Size_<int> pCellMaxSize, const cv::Size_<float> pCellStep)
{
    _cellMinSize = pCellMinSize;
    _cellMaxSize = pCellMaxSize;
    _cellStep = pCellStep;
}

/*************/
float Descriptor_Hog::getDescriptorNorm(vector<float> pDescriptor) const
{
    float norm;

    if (_normType == L1_NORM)
    {
        norm = _epsilon;
        for (int i = 0; i < _binsPerCell; ++i)
            norm += abs(pDescriptor[i]);
    }
    else if (_normType == L2_NORM)
    {
        norm = _epsilon;
        for (int i = 0; i < _binsPerCell; ++i)
            norm += pow(pDescriptor[i], 2.f);

        norm = sqrtf(norm);
    }

    return norm;
}

/*************/
float Descriptor_Hog::getGaussian(const float x, const float sigma) const
{
    static float sqrt2pi = sqrtf(2*CV_PI);
    float factor = pow(sigma * sqrt2pi, -1.f);
    return factor * exp(-pow(x, 2.f) / (2*pow(sigma, 2.f)));
}

