#ifndef EDGE_BASED_REGISTRATION_HPP
#define EDGE_BASED_REGISTRATION_HPP

#include <boost/iterator/counting_iterator.hpp>

#include "core/registration/correction.hpp"
#include "core/registration/edgebalancer.hpp"
#include "core/registration/elchcorrection.h"
#include "core/registration/errormetric.hpp"
#include "core/registration/icpregistration.h"
#include "core/registration/linearregistration.hpp"
#include "core/registration/lumcorrection.h"
#include "core/registration/registrationalgorithm.hpp"
#include "core/registration/sacregistration.h"
#include "io/pcdinputiterator.hpp"

class EdgeBasedRegistration : public RegistrationAlgorithm {
public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    struct Loop : RegistrationAlgorithm::Loop {
        std::pair<uint, uint> edge_frames_indexes;
        std::pair<Frame, Frame> edge_frames;
        KeypointsFrame edge_keypoints;
        std::pair<Eigen::Matrix4f, Eigen::Matrix4f> edge_transformations;

        Loop(const uint& start_loop_frame_index, const uint& end_loop_frame_index)
        {
            if (end_loop_frame_index - start_loop_frame_index == 0) {
                throw std::invalid_argument("Loop loop_size == 0");
            }

            edge_frames_indexes = std::make_pair(uint(start_loop_frame_index), uint(end_loop_frame_index));
            edge_transformations = std::make_pair(Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity());
        }
    };
    typedef std::vector<Loop, Eigen::aligned_allocator<Loop> > Loops;

    EdgeBasedRegistration(QObject* parent, QSettings* parent_settings)
        : RegistrationAlgorithm(parent, parent_settings)
        , loop_size(settings->value("ALGORITHM_SETTINGS/EDGE_BASED_RECONSTRUCTION_FIXED_STEP").toInt())
    {
    }

private:
    const int loop_size;
    Loops loops;

    void prepare_all_loops()
    {
        const uint read_loop_size = loop_size * read_step;

        Frames edge_frames;
        Frames transformed_edge_frames;

        if (!settings->value("ALGORITHM_SETTINGS/EDGE_BASED_RECONSTRUCTION_EDGE_BALANCING").toBool()) {
            for (uint i = read_from + read_loop_size; i <= read_to; i += read_loop_size) {
                loops.push_back(Loop(i - read_loop_size, i));
            }

            const uint edges_from = loops.front().edge_frames_indexes.first;
            const uint edges_to = loops.back().edge_frames_indexes.second;
            for (Iter it(settings, edges_from, edges_to, read_loop_size); it != Iter(); ++it) {
                edge_frames.push_back(*it);
            }
        } else {
            Iter it(settings, read_from, read_to, read_step);
            EdgeBalancer<CameraDistanceMetric, Iter> eb(it, Iter(), loop_size, settings);
            std::vector<uint> edge_indices = eb.balance();

            for (uint index = 0, edge_index = 0; it != Iter(); ++it, ++index) {
                if (index == edge_indices[edge_index]) {
                    edge_frames.push_back(*it);
                    ++edge_index;
                }
            }

            for (uint i = 1; i < edge_indices.size(); ++i) {
                const uint from = edge_indices[i - 1] * read_step + read_from;
                const uint to = edge_indices[i] * read_step + read_from;

                loops.push_back(Loop(from, to));
            }
        }

        if (loops.size() + 1 != edge_frames.size()) {
            throw std::runtime_error("EdgeBasedRegistration::prepare_all_loops loops.size() != edge_frames.size() + 1");
        }

        PcdFilters filters(this, settings);
        filters.setInput(edge_frames);
        filters.filter(edge_frames);

        LinearRegistration<SaCRegistration> linear_sac(this, settings);
        linear_sac.setInput(edge_frames, Eigen::Matrix4f::Identity());
        Matrix4fVector sac_t = linear_sac.align(transformed_edge_frames);

        LinearRegistration<ICPRegistration> linear_icp(this, settings);
        linear_icp.setInput(transformed_edge_frames, Eigen::Matrix4f::Identity());
        linear_icp.setKeypoints(linear_sac.getTransformedKeypoints());
        Matrix4fVector icp_t = linear_icp.align(transformed_edge_frames);

        KeypointsFrames kp = linear_sac.getKeypoints();
        for (uint i = 1; i < icp_t.size(); ++i) {
            loops[i - 1].edge_frames.first = edge_frames[i - 1];
            loops[i - 1].edge_frames.second = edge_frames[i];

            loops[i - 1].edge_transformations.first = icp_t[i - 1] * sac_t[i - 1];
            loops[i - 1].edge_transformations.second = icp_t[i] * sac_t[i];

            loops[i - 1].edge_keypoints = kp[i - 1];
        }
    }

    void process_all_loops()
    {
        for (uint i = 0; i < loops.size(); ++i) {
            loops[i] = process_one_loop(loops[i]);
        }

        loops_data_vizualization(loops);
    }

    void perform_tsdf_meshing()
    {
        Matrix4fVector result_t;
        for (uint i = 0; i < loops.size(); ++i) {
            std::copy(loops[i].inner_transformations.begin(), loops[i].inner_transformations.end(),
                std::back_inserter(result_t));
        }

        volumeReconstruction->prepareVolume();
        pcdVizualizer->redraw();

        if (settings->value("VISUALIZATION/DRAW_ALL_CAMERA_POSES").toBool()) {
            pcdVizualizer->visualizeCameraPoses(result_t);
        }

        if (settings->value("VISUALIZATION/CPU_TSDF_DRAW_MESH").toBool()) {
            pcl::PolygonMesh mesh;
            volumeReconstruction->calculateMesh();
            volumeReconstruction->getPoligonMesh(mesh);
            pcdVizualizer->visualizeMesh(mesh);
        }
    }

    Loop process_one_loop(const Loop& loop)
    {
        Frames inner_frames;
        Iter it(settings, loop.edge_frames_indexes.first, loop.edge_frames_indexes.second, read_step);
        for (; it != Iter(); ++it) {
            inner_frames.push_back(*it);
        }

        PcdFilters filters(this, settings);
        filters.setInput(inner_frames);
        filters.filter(inner_frames);

        Frames transformed_inner_frames;
        LinearRegistration<SaCRegistration> linear_sac(this, settings);
        linear_sac.setInput(inner_frames, loop.edge_transformations.first);
        const Matrix4fVector sac_t = linear_sac.align(transformed_inner_frames);

        LinearRegistration<ICPRegistration> linear_icp(this, settings);
        linear_icp.setInput(transformed_inner_frames, Eigen::Matrix4f::Identity());
        linear_icp.setKeypoints(linear_sac.getTransformedKeypoints());
        const Matrix4fVector icp_t = linear_icp.align(transformed_inner_frames);
        KeypointsFrames transformed_keypoints = linear_icp.getTransformedKeypoints();

        Matrix4fVector result_t;
        for (uint i = 0; i < icp_t.size(); ++i) {
            result_t.push_back(icp_t[i] * sac_t[i]);
        }

        if (settings->value("ALGORITHM_SETTINGS/EDGE_BASED_RECONSTRUCTION_ELCH_LUM").toBool()) {
            Correction<ElchCorrection> elch(this, settings);
            elch.setInput(transformed_inner_frames, linear_icp.getTransformedKeypoints(), result_t, loop.edge_keypoints);
            const Matrix4fVector elch_t = elch.correct(transformed_inner_frames);
            for (uint i = 1; i < elch_t.size(); ++i) {
                result_t[i] = elch_t[i] * result_t[i];
            }

            Correction<LumCorrection> lum(this, settings);
            lum.setInput(transformed_inner_frames, elch.getTransformedKeypoints(), result_t, loop.edge_keypoints);
            const Matrix4fVector lum_t = lum.correct(transformed_inner_frames);
            for (uint i = 1; i < lum_t.size(); ++i) {
                result_t[i] = lum_t[i] * result_t[i];
            }
            transformed_keypoints = lum.getTransformedKeypoints();
        }

        vizualization(inner_frames, transformed_inner_frames, transformed_keypoints, result_t);

        Loop result_loop(loop);
        result_loop.inner_transformations = result_t;
        result_loop.inner_t_fitness_scores = linear_icp.getFitnessScores();

        return result_loop;
    }
};

#endif //EDGE_BASED_REGISTRATION_HPP
