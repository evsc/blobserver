#include "hdribuilder.h"

using namespace std;
using namespace cv;

#define MAX_WEIGHT 1e10

/*************/
HdriBuilder::HdriBuilder()
{
    mContinuous = false;

    mMinSum = 0.1f;
    mMinExposureIndex = 0;
    mMaxExposureIndex = 0;

    mHDRi.create(0, 0, CV_32FC3);

    for (uchar i = 0; i < 255; ++i)
        mGaussianLUT[i] = getGaussian(i);
}

/*************/
HdriBuilder::~HdriBuilder()
{
}

/*************/
bool HdriBuilder::addLDR(const Mat& pImage, float pEV)
{
    LDRi lLDRi;

    lLDRi.EV = pEV;
    lLDRi.image = pImage;

    // Check if this is the first image
    if(mLDRi.size() == 0)
    {
        mLDRi.push_back(lLDRi);
        return true;
    }
    // If not, we must check the width and height
    // as they must be the same
    else
    {
        if(lLDRi.image.cols != mLDRi[0].image.cols)
        {
            return false;
        }
        if(lLDRi.image.rows != mLDRi[0].image.rows)
        {
            return false;
        }

        bool lResult = true;
        for(int i=0; i<(int)mLDRi.size(); i++)
        {
            if(mLDRi[i].EV == lLDRi.EV && mContinuous)
            {
                mLDRi[i] = lLDRi;
                lResult = false;
            }
            else if (mLDRi[i].EV == lLDRi.EV)
                lResult = false;
        }

        if(lResult)
            mLDRi.push_back(lLDRi);

        return lResult;
    }
}

/*************/
Mat HdriBuilder::getHDRI() const
{
    return mHDRi;
}

/*************/
bool HdriBuilder::computeHDRI()
{
    // If no LDRi were submitted
    if(mLDRi.size() == 0)
        return false;

    // HDRi the same size as LDRi, but RGB32f
    mHDRi.create(mLDRi[0].image.rows, mLDRi[0].image.cols, CV_32FC3);

    // Ordering images
    sort(mLDRi.begin(), mLDRi.end(), [&] (LDRi a, LDRi b)
    {
        return a.EV < b.EV;
    });

    // Convert all LDRi to floating, exposure corrected LDRi
    for_each (mLDRi.begin(), mLDRi.end(), [&] (LDRi& ldri)
    {
        if (ldri.imagef.total() > 0 && ldri.w.total() > 0)
            return;

        ldri.imagef = cv::Mat::zeros(ldri.image.rows, ldri.image.cols, CV_32FC3);
        ldri.w = cv::Mat::zeros(ldri.image.rows, ldri.image.cols, CV_32FC3);

        for (uint x = 0; x < (uint)mHDRi.cols; ++x)
            for (uint y = 0; y < (uint)mHDRi.rows; ++y)
            {
                unsigned char lLDRPixel;
                float lCoeff;

                for(unsigned int channel=0; channel<3; channel++)
                {
                    lLDRPixel = ldri.image.at<Vec3b>(y, x)[channel];
                    lCoeff = mGaussianLUT[lLDRPixel];
                    ldri.w.at<Vec3f>(y, x)[channel] = lCoeff;
                    ldri.imagef.at<Vec3f>(y, x)[channel] = lCoeff*(float)lLDRPixel/127.f*pow(2.0f, ldri.EV);
                }
            }
    });

    // Calculation of each HDR pixel
    for(unsigned int x=0; x<(unsigned int)mHDRi.cols; x++)
    {
        for(unsigned int y=0; y<(unsigned int)mHDRi.rows; y++)
        {
            float lHDRPixel[3], lSum[3];
            lHDRPixel[0] = 0.f;
            lHDRPixel[1] = 0.f;
            lHDRPixel[2] = 0.f;
            lSum[0] = 0.f;
            lSum[1] = 0.f;
            lSum[2] = 0.f;

            // If the least exposed channel is overexposed on one channel
            // we set the pixel to white
            if(mLDRi[mLDRi.size()-1].image.at<Vec3b>(y, x)[0] == 255
                    || mLDRi[mLDRi.size()-1].image.at<Vec3b>(y, x)[1] == 255
                    || mLDRi[mLDRi.size()-1].image.at<Vec3b>(y, x)[2] == 255)
            {
                for(unsigned char channel=0; channel<3; channel++)
                {
                    lHDRPixel[channel] = 255.f/127.f*pow(2.0f, mLDRi[mLDRi.size()-1].EV);
                    lSum[channel] = 1.f;
                }
            }
            // If the most exposed channel is underexposed on one channel
            // we set the pixel to (almost) black
            else if(mLDRi[0].image.at<Vec3b>(y, x)[0] < 64
                    && mLDRi[0].image.at<Vec3b>(y, x)[1] < 64
                    && mLDRi[0].image.at<Vec3b>(y, x)[2] < 64)
            {
                // We will stick to N&B in this case
                float lValue = 0.263f*(float)mLDRi[0].image.at<Vec3b>(y, x)[0]
                        + 0.655f*(float)mLDRi[0].image.at<Vec3b>(y, x)[1]
                        + 0.082f*(float)mLDRi[0].image.at<Vec3b>(y, x)[2];
                for(unsigned char channel=0; channel<3; channel++)
                {
                    lHDRPixel[channel] = lValue/127.f*pow(2.0f, mLDRi[0].EV);
                    lSum[channel] = 1.f;
                }
            }
            // else, we add the contribution of each image
            else
            {
                for(unsigned int index=0; index<mLDRi.size(); index++)
                    for(unsigned int channel=0; channel<3; channel++)
                    {
                        lHDRPixel[channel] += mLDRi[index].imagef.at<Vec3f>(y, x)[channel];
                        lSum[channel] += mLDRi[index].w.at<Vec3f>(y, x)[channel];
                    }
            }

            // We divide by the sum of gaussians to get the final values
            lHDRPixel[0] /= lSum[0];
            lHDRPixel[1] /= lSum[1];
            lHDRPixel[2] /= lSum[2];

            mHDRi.at<Vec3f>(y, x)[0] = lHDRPixel[0];
            mHDRi.at<Vec3f>(y, x)[1] = lHDRPixel[1];
            mHDRi.at<Vec3f>(y, x)[2] = lHDRPixel[2];
        }
    }

    if (!mContinuous)
        mLDRi.clear();

    return true;
}

/*************/
float HdriBuilder::getGaussian(unsigned char pValue) const
{
    float lSigma = 8;
    float lMu = 127;

    float lValue = 1/(lSigma*2*M_PI)*exp(-(pValue-lMu)*(pValue-lMu)/(2*pow(lSigma,2)));
    // We want the mean value (lMu) to return 1
    lValue /= 1/(lSigma*2*M_PI);

    return lValue;
}
