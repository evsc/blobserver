/*
 * Copyright (C) 2012 Emmanuel Durand
 *
 * This file is part of blobserver.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * blobserver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with blobserver.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * @actuator_objOnAPlane.h
 * The Actuator_ObjOnAPlane class.
 */

#ifndef OBJONAPLANE_H
#define OBJONAPLANE_H

#include <memory>
#include "actuator.h"
#include "blob_2D.h"

class Actuator_ObjOnAPlane : public Actuator
{
    public:
        Actuator_ObjOnAPlane();
        Actuator_ObjOnAPlane(int pParam);

        static std::string getClassName() {return mClassName;}
        static std::string getDocumentation() {return mDocumentation;}

        atom::Message detect(const std::vector< Capture_Ptr > pCaptures);
        void setParameter(atom::Message pMessage);

    private:
        static std::string mClassName;
        static std::string mDocumentation;
        static unsigned int mSourceNbr;

        int mMaxTrackedBlobs;
        float mDetectionLevel;
        int mFilterSize;
        float mProcessNoiseCov, mMeasurementNoiseCov;
        
        std::vector<Blob2D> mBlobs; // Vector of detected and tracked blobs
        int mMinArea;

        std::vector<std::vector<cv::Vec2f>> mSpaces; // First space is the real plane
        std::vector<cv::Mat> mMaps;
        bool mMapsUpdated;

        void make(); // Called by the constructor
        void updateMaps(std::vector<cv::Mat> pCaptures); // Updates the space conversion maps
};

REGISTER_ACTUATOR(Actuator_ObjOnAPlane)

 #endif // OBJONAPLANE_H
