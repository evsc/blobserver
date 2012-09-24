#include <iostream>
#include "glib.h"
#include "opencv2/opencv.hpp"
#include "lo/lo.h"

static gboolean gHide = FALSE;
static gint gCamNbr = 0;
static gint gFilterSize = 3;
static GString* gIpAddress = NULL;
static GString* gIpPort = NULL;
static gboolean gVerbose = FALSE;

static GOptionEntry gEntries[] =
{
    {"hide", 0, 0, G_OPTION_ARG_NONE, &gHide, "Hides the camera output", NULL},
    {"cam", 'c', 0, G_OPTION_ARG_INT, &gCamNbr, "Selects which camera to use", NULL},
    {"filter", 'f', 0, G_OPTION_ARG_INT, &gFilterSize, "Specifies the size of the filtering kernel to use", NULL},
    {"ip", 'i', 0, G_OPTION_ARG_STRING_ARRAY, &gIpAddress, "Specifies the ip address to send messages to", NULL}, 
    {"port", 'p', 0, G_OPTION_ARG_STRING_ARRAY, &gIpPort, "Specifies the port to send messages to", NULL},
    {"verbose", 'v', 0, G_OPTION_ARG_NONE, &gVerbose, "If set, outputs values to the std::out", NULL},
    {NULL}
};

/*****************/
class App
{
    public:
        // Constructor and destructor
        App();
        ~App();

        // Arguments parser
        int parseArgs(int argc, char **argv);

        // Initialization, depending on arguments
        int init();

        // Main loop
        int loop();

    private:
        // Attributes
        // liblo related
        lo_address mOscAddress;

        // opencv related
        cv::VideoCapture mCamera;
        cv::Mat mCameraBuffer;

        // Methods
};


/*****************/
App::App()
{
}


/*****************/
App::~App()
{
}

/*****************/
int App::parseArgs(int argc, char** argv)
{
    GError *error = NULL;
    GOptionContext* context;

    context = g_option_context_new("- blobserver, sends blobs through OSC");
    g_option_context_add_main_entries(context, gEntries, NULL);

    if(!g_option_context_parse(context, &argc, &argv, &error))
    {
        std::cout << "Error while parsing options: " << error->message << std::endl;
        return 1;
    }

    return 0;
}

/*****************/
int App::init()
{
    // Initialize camera
    if(!mCamera.open(gCamNbr))
    {
        std::cout << "Error while opening camera number " << gCamNbr << ". Exiting." << std::endl;
        return 1;
    }
    // Get a first frame to initialize the buffer
    mCamera.read(mCameraBuffer);

    // Initialize OSC
    if(gIpAddress != NULL)
    {
        std::cout << "IP specified: " << gIpAddress->str << std::endl;
    }
    else
    {
        std::cout << "No IP specified, using localhost" << std::endl;
    }

    if(gIpPort != NULL)
    {
        std::cout << "Using port number " << gIpPort->str << std::endl;
    }
    else
    {
        gIpPort = g_string_new("9000");
        std::cout << "No port specified, using 9000" << std::endl;
    }

    mOscAddress = lo_address_new(gIpAddress->str, gIpPort->str);

    return 0;
}

/*****************/
int App::loop()
{
    cv::Mat lMean, lStdDev;
    cv::Mat lOutlier, lEroded, lFiltered;

    bool lShowCamera = !gHide;
    bool lShowOutliers = false;

    bool loop = true;
    while(loop)
    {
        // Frame capture
        mCamera.read(mCameraBuffer);

        // If the frame seems valid
        if(mCameraBuffer.size[0] > 0 && mCameraBuffer.size[0] > 0)
        {
            if(gHide == FALSE)
                cv::imshow("blobserver", mCameraBuffer);

            // Eliminate the outliers : calculate the mean and std dev
            lOutlier = cv::Mat::zeros(mCameraBuffer.size[0], mCameraBuffer.size[1], CV_8U);
            lEroded = lOutlier.clone();
            lFiltered = lOutlier.clone();
            cv::cvtColor(mCameraBuffer, lOutlier, CV_RGB2GRAY);

            cv::meanStdDev(mCameraBuffer, lMean, lStdDev);
            cv::absdiff(lOutlier, lMean.at<double>(0), lOutlier);

            // Detect pixels far from the mean (> 2*stddev)
            cv::threshold(lOutlier, lOutlier, 2*lStdDev.at<double>(0), 255, cv::THRESH_BINARY);

            // Erode and dilate to suppress noise
            cv::erode(lOutlier, lEroded, cv::Mat(), cv::Point(-1, -1), gFilterSize);
            cv::dilate(lEroded, lFiltered, cv::Mat(), cv::Point(-1, -1), gFilterSize);

            if(lShowOutliers)
                cv::imshow("Barycenter", lFiltered);

            // Calculate the barycenter of the outliers
            int lNumber = 0;
            int lX=0, lY=0;

            for(int x=0; x<lFiltered.size[1]; x++)
                for(int y=0; y<lFiltered.size[0]; y++)
                {
                    if(lFiltered.at<uchar>(y, x) == 255)
                    {
                        lX += x;
                        lY += y;
                        lNumber++;
                    }
                }

            if(lNumber > 0)
            {
                lX /= lNumber;
                lY /= lNumber;
            }
            else
            {
                lX = lFiltered.size[1] / 2;
                lY = lFiltered.size[0] / 2;
            }

            if(gVerbose)
                std::cout << "x: " << lX << " - y: " << lY << " - size: " << lNumber << std::endl;

            // Send the result
            lo_send(mOscAddress, "/blobserver/", "iii", lX, lY, lNumber);
        }

        char lKey = cv::waitKey(5);
        if(lKey == 'q')
            loop = false;
        if(lKey == 'w')
        {
            lShowCamera = !lShowCamera;
            lShowOutliers = !lShowOutliers;
        }
    }

    return 0;
}

/*****************/
int main(int argc, char** argv)
{
    App theApp;
    int ret;

    ret = theApp.parseArgs(argc, argv);
    if(ret != 0)
        return ret;

    ret = theApp.init();
    if(ret != 0)
        return ret;

    ret = theApp.loop();
    return ret;
}
