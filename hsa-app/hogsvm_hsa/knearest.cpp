/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000, Intel Corporation, all rights reserved.
// Copyright (C) 2014, Itseez Inc, all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "kdtree.hpp"
#include "precomp.hpp"

/****************************************************************************************\
*                              K-Nearest Neighbors Classifier                            *
\****************************************************************************************/

namespace cv {
namespace hsaml {

KNearest::Params::Params(int k, bool isclassifier_, int Emax_, int algorithmType_) :
    defaultK(k),
    isclassifier(isclassifier_),
    Emax(Emax_),
    algorithmType(algorithmType_)
{
}

class KNearestImpl : public KNearest
{
public:
    KNearestImpl(const Params& p)
    {
        params = p;
    }

    virtual ~KNearestImpl() {}

    Params getParams() const { return params; }
    void setParams(const Params& p) { params = p; }

    bool isClassifier() const { return params.isclassifier; }
    bool isTrained() const { return !samples.empty(); }

    String getDefaultModelName() const { return "opencv_ml_knn"; }

    void clear()
    {
        samples.release();
        responses.release();
    }

    int getVarCount() const { return samples.cols; }

    bool train( const Ptr<TrainData>& data, int flags )
    {
        Mat new_samples = data->getTrainSamples(ROW_SAMPLE);
        Mat new_responses;
        data->getTrainResponses().convertTo(new_responses, CV_32F);
        bool update = (flags & UPDATE_MODEL) != 0 && !samples.empty();

        CV_Assert( new_samples.type() == CV_32F );

        if( !update )
        {
            clear();
        }
        else
        {
            CV_Assert( new_samples.cols == samples.cols &&
                       new_responses.cols == responses.cols );
        }

        samples.push_back(new_samples);
        responses.push_back(new_responses);

        return true;
    }

    void findNearestCore( const Mat& _samples, int k0, const Range& range,
                          Mat* results, Mat* neighbor_responses,
                          Mat* dists, float* presult ) const
    {
        int testidx, baseidx, i, j, d = samples.cols, nsamples = samples.rows;
        int testcount = range.end - range.start;
        int k = std::min(k0, nsamples);

        AutoBuffer<float> buf(testcount*k*2);
        float* dbuf = buf;
        float* rbuf = dbuf + testcount*k;

        const float* rptr = responses.ptr<float>();

        for( testidx = 0; testidx < testcount; testidx++ )
        {
            for( i = 0; i < k; i++ )
            {
                dbuf[testidx*k + i] = FLT_MAX;
                rbuf[testidx*k + i] = 0.f;
            }
        }

        for( baseidx = 0; baseidx < nsamples; baseidx++ )
        {
            for( testidx = 0; testidx < testcount; testidx++ )
            {
                const float* v = samples.ptr<float>(baseidx);
                const float* u = _samples.ptr<float>(testidx + range.start);

                float s = 0;
                for( i = 0; i <= d - 4; i += 4 )
                {
                    float t0 = u[i] - v[i], t1 = u[i+1] - v[i+1];
                    float t2 = u[i+2] - v[i+2], t3 = u[i+3] - v[i+3];
                    s += t0*t0 + t1*t1 + t2*t2 + t3*t3;
                }

                for( ; i < d; i++ )
                {
                    float t0 = u[i] - v[i];
                    s += t0*t0;
                }

                Cv32suf si;
                si.f = (float)s;
                Cv32suf* dd = (Cv32suf*)(&dbuf[testidx*k]);
                float* nr = &rbuf[testidx*k];

                for( i = k; i > 0; i-- )
                    if( si.i >= dd[i-1].i )
                        break;
                if( i >= k )
                    continue;

                for( j = k-2; j >= i; j-- )
                {
                    dd[j+1].i = dd[j].i;
                    nr[j+1] = nr[j];
                }
                dd[i].i = si.i;
                nr[i] = rptr[baseidx];
            }
        }

        float result = 0.f;
        float inv_scale = 1.f/k;

        for( testidx = 0; testidx < testcount; testidx++ )
        {
            if( neighbor_responses )
            {
                float* nr = neighbor_responses->ptr<float>(testidx + range.start);
                for( j = 0; j < k; j++ )
                    nr[j] = rbuf[testidx*k + j];
                for( ; j < k0; j++ )
                    nr[j] = 0.f;
            }

            if( dists )
            {
                float* dptr = dists->ptr<float>(testidx + range.start);
                for( j = 0; j < k; j++ )
                    dptr[j] = dbuf[testidx*k + j];
                for( ; j < k0; j++ )
                    dptr[j] = 0.f;
            }

            if( results || testidx+range.start == 0 )
            {
                if( !params.isclassifier || k == 1 )
                {
                    float s = 0.f;
                    for( j = 0; j < k; j++ )
                        s += rbuf[testidx*k + j];
                    result = (float)(s*inv_scale);
                }
                else
                {
                    float* rp = rbuf + testidx*k;
                    for( j = k-1; j > 0; j-- )
                    {
                        bool swap_fl = false;
                        for( i = 0; i < j; i++ )
                        {
                            if( rp[i] > rp[i+1] )
                            {
                                std::swap(rp[i], rp[i+1]);
                                swap_fl = true;
                            }
                        }
                        if( !swap_fl )
                            break;
                    }

                    result = rp[0];
                    int prev_start = 0;
                    int best_count = 0;
                    for( j = 1; j <= k; j++ )
                    {
                        if( j == k || rp[j] != rp[j-1] )
                        {
                            int count = j - prev_start;
                            if( best_count < count )
                            {
                                best_count = count;
                                result = rp[j-1];
                            }
                            prev_start = j;
                        }
                    }
                }
                if( results )
                    results->at<float>(testidx + range.start) = result;
                if( presult && testidx+range.start == 0 )
                    *presult = result;
            }
        }
    }

    struct findKNearestInvoker : public ParallelLoopBody
    {
        findKNearestInvoker(const KNearestImpl* _p, int _k, const Mat& __samples,
                            Mat* __results, Mat* __neighbor_responses, Mat* __dists, float* _presult)
        {
            p = _p;
            k = _k;
            _samples = &__samples;
            _results = __results;
            _neighbor_responses = __neighbor_responses;
            _dists = __dists;
            presult = _presult;
        }

        void operator()( const Range& range ) const
        {
            int delta = std::min(range.end - range.start, 256);
            for( int start = range.start; start < range.end; start += delta )
            {
                p->findNearestCore( *_samples, k, Range(start, std::min(start + delta, range.end)),
                                    _results, _neighbor_responses, _dists, presult );
            }
        }

        const KNearestImpl* p;
        int k;
        const Mat* _samples;
        Mat* _results;
        Mat* _neighbor_responses;
        Mat* _dists;
        float* presult;
    };

    float findNearest( InputArray _samples, int k,
                       OutputArray _results,
                       OutputArray _neighborResponses,
                       OutputArray _dists ) const
    {
        float result = 0.f;
        CV_Assert( 0 < k );

        Mat test_samples = _samples.getMat();
        CV_Assert( test_samples.type() == CV_32F && test_samples.cols == samples.cols );
        int testcount = test_samples.rows;

        if( testcount == 0 )
        {
            _results.release();
            _neighborResponses.release();
            _dists.release();
            return 0.f;
        }

        Mat res, nr, d, *pres = 0, *pnr = 0, *pd = 0;
        if( _results.needed() )
        {
            _results.create(testcount, 1, CV_32F);
            pres = &(res = _results.getMat());
        }
        if( _neighborResponses.needed() )
        {
            _neighborResponses.create(testcount, k, CV_32F);
            pnr = &(nr = _neighborResponses.getMat());
        }
        if( _dists.needed() )
        {
            _dists.create(testcount, k, CV_32F);
            pd = &(d = _dists.getMat());
        }

        findKNearestInvoker invoker(this, k, test_samples, pres, pnr, pd, &result);
        parallel_for_(Range(0, testcount), invoker);
        //invoker(Range(0, testcount));
        return result;
    }

    float predict(InputArray inputs, OutputArray outputs, int) const
    {
        return findNearest( inputs, params.defaultK, outputs, noArray(), noArray() );
    }

    void write( FileStorage& fs ) const
    {
        fs << "is_classifier" << (int)params.isclassifier;
        fs << "default_k" << params.defaultK;

        fs << "samples" << samples;
        fs << "responses" << responses;
    }

    void read( const FileNode& fn )
    {
        clear();
        params.isclassifier = (int)fn["is_classifier"] != 0;
        params.defaultK = (int)fn["default_k"];

        fn["samples"] >> samples;
        fn["responses"] >> responses;
    }

    Mat samples;
    Mat responses;
    Params params;
};


class KNearestKDTreeImpl : public KNearest
{
public:
    KNearestKDTreeImpl(const Params& p)
    {
        params = p;
    }

    virtual ~KNearestKDTreeImpl() {}

    Params getParams() const { return params; }
    void setParams(const Params& p) { params = p; }

    bool isClassifier() const { return params.isclassifier; }
    bool isTrained() const { return !samples.empty(); }

    String getDefaultModelName() const { return "opencv_ml_knn_kd"; }

    void clear()
    {
        samples.release();
        responses.release();
    }

    int getVarCount() const { return samples.cols; }

    bool train( const Ptr<TrainData>& data, int flags )
    {
        Mat new_samples = data->getTrainSamples(ROW_SAMPLE);
        Mat new_responses;
        data->getTrainResponses().convertTo(new_responses, CV_32F);
        bool update = (flags & UPDATE_MODEL) != 0 && !samples.empty();

        CV_Assert( new_samples.type() == CV_32F );

        if( !update )
        {
            clear();
        }
        else
        {
            CV_Assert( new_samples.cols == samples.cols &&
                       new_responses.cols == responses.cols );
        }

        samples.push_back(new_samples);
        responses.push_back(new_responses);

        tr.build(samples);

        return true;
    }

    float findNearest( InputArray _samples, int k,
                       OutputArray _results,
                       OutputArray _neighborResponses,
                       OutputArray _dists ) const
    {
        float result = 0.f;
        CV_Assert( 0 < k );

        Mat test_samples = _samples.getMat();
        CV_Assert( test_samples.type() == CV_32F && test_samples.cols == samples.cols );
        int testcount = test_samples.rows;

        if( testcount == 0 )
        {
            _results.release();
            _neighborResponses.release();
            _dists.release();
            return 0.f;
        }

        Mat res, nr, d;
        if( _results.needed() )
        {
            _results.create(testcount, 1, CV_32F);
            res = _results.getMat();
        }
        if( _neighborResponses.needed() )
        {
            _neighborResponses.create(testcount, k, CV_32F);
            nr = _neighborResponses.getMat();
        }
        if( _dists.needed() )
        {
            _dists.create(testcount, k, CV_32F);
            d = _dists.getMat();
        }

        for (int i=0; i<test_samples.rows; ++i)
        {
            Mat _res, _nr, _d;
            if (res.rows>i)
            {
                _res = res.row(i);
            }
            if (nr.rows>i)
            {
                _nr = nr.row(i);
            }
            if (d.rows>i)
            {
                _d = d.row(i);
            }
            tr.findNearest(test_samples.row(i), k, params.Emax, _res, _nr, _d, noArray());
        }

        return result; // currently always 0
    }

    float predict(InputArray inputs, OutputArray outputs, int) const
    {
        return findNearest( inputs, params.defaultK, outputs, noArray(), noArray() );
    }

    void write( FileStorage& fs ) const
    {
        fs << "is_classifier" << (int)params.isclassifier;
        fs << "default_k" << params.defaultK;

        fs << "samples" << samples;
        fs << "responses" << responses;
    }

    void read( const FileNode& fn )
    {
        clear();
        params.isclassifier = (int)fn["is_classifier"] != 0;
        params.defaultK = (int)fn["default_k"];

        fn["samples"] >> samples;
        fn["responses"] >> responses;
    }

    KDTree tr;

    Mat samples;
    Mat responses;
    Params params;
};

Ptr<KNearest> KNearest::create(const Params& p)
{
    if (KDTREE==p.algorithmType)
    {
        return makePtr<KNearestKDTreeImpl>(p);
    }

    return makePtr<KNearestImpl>(p);
}

}
}

/* End of file */
