/*!
 * Copyright by Contributors 2017-2021
 */
#include <dmlc/omp.h>
#include <dmlc/any.h>

#include <cstddef>
#include <limits>
#include <mutex>

#include "xgboost/base.h"
#include "xgboost/data.h"
#include "xgboost/predictor.h"
#include "xgboost/tree_model.h"
#include "xgboost/tree_updater.h"
#include "xgboost/logging.h"
#include "xgboost/host_device_vector.h"

#include "../data/adapter.h"
#include "../common/math.h"
#include "../common/threading_utils.h"
#include "../gbm/gbtree_model.h"

namespace xgboost {
namespace predictor {

DMLC_REGISTRY_FILE_TAG(cpu_predictor);

bst_float PredValue(const SparsePage::Inst &inst,
                    const std::vector<std::unique_ptr<RegTree>> &trees,
                    const std::vector<int> &tree_info, int bst_group,
                    RegTree::FVec *p_feats, unsigned tree_begin,
                    unsigned tree_end) {
  bst_float psum = 0.0f;
  p_feats->Fill(inst);
  for (size_t i = tree_begin; i < tree_end; ++i) {
    if (tree_info[i] == bst_group) {
      int tid = trees[i]->GetLeafIndex(*p_feats);
      psum += (*trees[i])[tid].LeafValue();
    }
  }
  p_feats->Drop(inst);
  return psum;
}

inline bst_float PredValueByOneTree(const RegTree::FVec& p_feats,
                                    const std::unique_ptr<RegTree>& tree) {
  const int lid = p_feats.HasMissing() ? tree->GetLeafIndex<true>(p_feats) :
                                         tree->GetLeafIndex<false>(p_feats);  // 35% speed up
  return (*tree)[lid].LeafValue();
}

inline void PredictByAllTrees(gbm::GBTreeModel const &model, const size_t tree_begin,
                              const size_t tree_end, std::vector<bst_float>* out_preds,
                              const size_t predict_offset, const size_t num_group,
                              const std::vector<RegTree::FVec> &thread_temp,
                              const size_t offset, const size_t block_size) {
  std::vector<bst_float> &preds = *out_preds;
  for (size_t tree_id = tree_begin; tree_id < tree_end; ++tree_id) {
    const size_t gid = model.tree_info[tree_id];
    for (size_t i = 0; i < block_size; ++i) {
      preds[(predict_offset + i) * num_group + gid] += PredValueByOneTree(thread_temp[offset + i],
                                                                      model.trees[tree_id]);
    }
  }
}

template <typename DataView>
void FVecFill(const size_t block_size, const size_t batch_offset, const int num_feature,
              DataView* batch, const size_t fvec_offset, std::vector<RegTree::FVec>* p_feats) {
  for (size_t i = 0; i < block_size; ++i) {
    RegTree::FVec &feats = (*p_feats)[fvec_offset + i];
    if (feats.Size() == 0) {
      feats.Init(num_feature);
    }
    const SparsePage::Inst inst = (*batch)[batch_offset + i];
    feats.Fill(inst);
  }
}
template <typename DataView>
void FVecDrop(const size_t block_size, const size_t batch_offset, DataView* batch,
              const size_t fvec_offset, std::vector<RegTree::FVec>* p_feats) {
  for (size_t i = 0; i < block_size; ++i) {
    RegTree::FVec &feats = (*p_feats)[fvec_offset + i];
    const SparsePage::Inst inst = (*batch)[batch_offset + i];
    feats.Drop(inst);
  }
}

template <size_t kUnrollLen = 8>
struct SparsePageView {
  bst_row_t base_rowid;
  HostSparsePageView view;
  static size_t constexpr kUnroll = kUnrollLen;

  explicit SparsePageView(SparsePage const *p)
      : base_rowid{p->base_rowid} {
    view = p->GetView();
  }
  SparsePage::Inst operator[](size_t i) { return view[i]; }
  size_t Size() const { return view.Size(); }
};

template <typename Adapter, size_t kUnrollLen = 8>
class AdapterView {
  Adapter* adapter_;
  float missing_;
  common::Span<Entry> workspace_;
  std::vector<size_t> current_unroll_;

 public:
  static size_t constexpr kUnroll = kUnrollLen;

 public:
  explicit AdapterView(Adapter *adapter, float missing,
                       common::Span<Entry> workplace)
      : adapter_{adapter}, missing_{missing}, workspace_{workplace},
        current_unroll_(omp_get_max_threads() > 0 ? omp_get_max_threads() : 1, 0) {}
  SparsePage::Inst operator[](size_t i) {
    bst_feature_t columns = adapter_->NumColumns();
    auto const &batch = adapter_->Value();
    auto row = batch.GetLine(i);
    auto t = omp_get_thread_num();
    auto const beg = (columns * kUnroll * t) + (current_unroll_[t] * columns);
    size_t non_missing {beg};
    for (size_t c = 0; c < row.Size(); ++c) {
      auto e = row.GetElement(c);
      if (missing_ != e.value && !common::CheckNAN(e.value)) {
        workspace_[non_missing] =
            Entry{static_cast<bst_feature_t>(e.column_idx), e.value};
        ++non_missing;
      }
    }
    auto ret = workspace_.subspan(beg, non_missing - beg);
    current_unroll_[t]++;
    if (current_unroll_[t] == kUnroll) {
      current_unroll_[t] = 0;
    }
    return ret;
  }

  size_t Size() const { return adapter_->NumRows(); }

  bst_row_t const static base_rowid = 0;  // NOLINT
};

template <typename DataView, size_t block_of_rows_size>
void PredictBatchByBlockOfRowsKernel(DataView batch, std::vector<bst_float> *out_preds,
                                     gbm::GBTreeModel const &model, int32_t tree_begin,
                                     int32_t tree_end,
                                     std::vector<RegTree::FVec> *p_thread_temp) {
  auto& thread_temp = *p_thread_temp;
  int32_t const num_group = model.learner_model_param->num_output_group;

  CHECK_EQ(model.param.size_leaf_vector, 0)
      << "size_leaf_vector is enforced to 0 so far";
  // parallel over local batch
  const auto nsize = static_cast<bst_omp_uint>(batch.Size());
  const int num_feature = model.learner_model_param->num_feature;
  const bst_omp_uint n_row_blocks = (nsize) / block_of_rows_size + !!((nsize) % block_of_rows_size);
  common::ParallelFor(n_row_blocks, [&](bst_omp_uint block_id) {
    const size_t batch_offset = block_id * block_of_rows_size;
    const size_t block_size = std::min(nsize - batch_offset, block_of_rows_size);
    const size_t fvec_offset = omp_get_thread_num() * block_of_rows_size;

    FVecFill(block_size, batch_offset, num_feature, &batch, fvec_offset, p_thread_temp);
    // process block of rows through all trees to keep cache locality
    PredictByAllTrees(model, tree_begin, tree_end, out_preds, batch_offset + batch.base_rowid,
                      num_group, thread_temp, fvec_offset, block_size);
    FVecDrop(block_size, batch_offset, &batch, fvec_offset, p_thread_temp);
  });
}

class CPUPredictor : public Predictor {
 protected:
  // init thread buffers
  static void InitThreadTemp(int nthread, int num_feature, std::vector<RegTree::FVec>* out) {
    int prev_thread_temp_size = out->size();
    if (prev_thread_temp_size < nthread) {
      out->resize(nthread, RegTree::FVec());
    }
  }

  void PredictDMatrix(DMatrix *p_fmat, std::vector<bst_float> *out_preds,
                      gbm::GBTreeModel const &model, int32_t tree_begin,
                      int32_t tree_end) const {
    const int threads = omp_get_max_threads();
    std::vector<RegTree::FVec> feat_vecs;
    InitThreadTemp(threads * kBlockOfRowsSize,
                   model.learner_model_param->num_feature, &feat_vecs);
    for (auto const& batch : p_fmat->GetBatches<SparsePage>()) {
      CHECK_EQ(out_preds->size(),
               p_fmat->Info().num_row_ * model.learner_model_param->num_output_group);
      size_t constexpr kUnroll = 8;
      PredictBatchByBlockOfRowsKernel<SparsePageView<kUnroll>,
                          kBlockOfRowsSize>(SparsePageView<kUnroll>{&batch},
                                              out_preds, model, tree_begin,
                                              tree_end, &feat_vecs);
    }
  }

  void InitOutPredictions(const MetaInfo& info,
                          HostDeviceVector<bst_float>* out_preds,
                          const gbm::GBTreeModel& model) const override {
    CHECK_NE(model.learner_model_param->num_output_group, 0);
    size_t n = model.learner_model_param->num_output_group * info.num_row_;
    const auto& base_margin = info.base_margin_.HostVector();
    out_preds->Resize(n);
    std::vector<bst_float>& out_preds_h = out_preds->HostVector();
    if (base_margin.size() == n) {
      CHECK_EQ(out_preds->Size(), n);
      std::copy(base_margin.begin(), base_margin.end(), out_preds_h.begin());
    } else {
      if (!base_margin.empty()) {
        std::ostringstream oss;
        oss << "Ignoring the base margin, since it has incorrect length. "
            << "The base margin must be an array of length ";
        if (model.learner_model_param->num_output_group > 1) {
          oss << "[num_class] * [number of data points], i.e. "
              << model.learner_model_param->num_output_group << " * " << info.num_row_
              << " = " << n << ". ";
        } else {
          oss << "[number of data points], i.e. " << info.num_row_ << ". ";
        }
        oss << "Instead, all data points will use "
            << "base_score = " << model.learner_model_param->base_score;
        LOG(WARNING) << oss.str();
      }
      std::fill(out_preds_h.begin(), out_preds_h.end(),
                model.learner_model_param->base_score);
    }
  }

 public:
  explicit CPUPredictor(GenericParameter const* generic_param) :
      Predictor::Predictor{generic_param} {}

  void PredictBatch(DMatrix *dmat, PredictionCacheEntry *predts,
                    const gbm::GBTreeModel &model, uint32_t tree_begin,
                    uint32_t tree_end = 0) const override {
    auto* out_preds = &predts->predictions;
    // This is actually already handled in gbm, but large amount of tests rely on the
    // behaviour.
    if (tree_end == 0) {
      tree_end = model.trees.size();
    }
    this->PredictDMatrix(dmat, &out_preds->HostVector(), model, tree_begin,
                         tree_end);
  }

  template <typename Adapter>
  void DispatchedInplacePredict(dmlc::any const &x, std::shared_ptr<DMatrix> p_m,
                                const gbm::GBTreeModel &model, float missing,
                                PredictionCacheEntry *out_preds,
                                uint32_t tree_begin, uint32_t tree_end) const {
    auto threads = omp_get_max_threads();
    auto m = dmlc::get<std::shared_ptr<Adapter>>(x);
    CHECK_EQ(m->NumColumns(), model.learner_model_param->num_feature)
        << "Number of columns in data must equal to trained model.";
    if (p_m) {
      p_m->Info().num_row_ = m->NumRows();
      this->InitOutPredictions(p_m->Info(), &(out_preds->predictions), model);
    } else {
      MetaInfo info;
      info.num_row_ = m->NumRows();
      this->InitOutPredictions(info, &(out_preds->predictions), model);
    }
    std::vector<Entry> workspace(m->NumColumns() * 8 * threads);
    auto &predictions = out_preds->predictions.HostVector();
    std::vector<RegTree::FVec> thread_temp;
    InitThreadTemp(threads * kBlockOfRowsSize,
                   model.learner_model_param->num_feature, &thread_temp);
    PredictBatchByBlockOfRowsKernel<AdapterView<Adapter>, kBlockOfRowsSize>(
        AdapterView<Adapter>(m.get(), missing, common::Span<Entry>{workspace}),
        &predictions, model, tree_begin, tree_end, &thread_temp);
  }

  bool InplacePredict(dmlc::any const &x, std::shared_ptr<DMatrix> p_m,
                      const gbm::GBTreeModel &model, float missing,
                      PredictionCacheEntry *out_preds, uint32_t tree_begin,
                      unsigned tree_end) const override {
    if (x.type() == typeid(std::shared_ptr<data::DenseAdapter>)) {
      this->DispatchedInplacePredict<data::DenseAdapter>(
          x, p_m, model, missing, out_preds, tree_begin, tree_end);
    } else if (x.type() == typeid(std::shared_ptr<data::CSRAdapter>)) {
      this->DispatchedInplacePredict<data::CSRAdapter>(
          x, p_m, model, missing, out_preds, tree_begin, tree_end);
    } else if (x.type() == typeid(std::shared_ptr<data::ArrayAdapter>)) {
      this->DispatchedInplacePredict<data::ArrayAdapter> (
          x, p_m, model, missing, out_preds, tree_begin, tree_end);
    } else if (x.type() == typeid(std::shared_ptr<data::CSRArrayAdapter>)) {
      this->DispatchedInplacePredict<data::CSRArrayAdapter> (
          x, p_m, model, missing, out_preds, tree_begin, tree_end);
    } else {
      return false;
    }
    return true;
  }

  void PredictInstance(const SparsePage::Inst& inst,
                       std::vector<bst_float>* out_preds,
                       const gbm::GBTreeModel& model, unsigned ntree_limit) const override {
    std::vector<RegTree::FVec> feat_vecs;
    feat_vecs.resize(1, RegTree::FVec());
    feat_vecs[0].Init(model.learner_model_param->num_feature);
    ntree_limit *= model.learner_model_param->num_output_group;
    if (ntree_limit == 0 || ntree_limit > model.trees.size()) {
      ntree_limit = static_cast<unsigned>(model.trees.size());
    }
    out_preds->resize(model.learner_model_param->num_output_group *
                      (model.param.size_leaf_vector + 1));
    // loop over output groups
    for (uint32_t gid = 0; gid < model.learner_model_param->num_output_group; ++gid) {
      (*out_preds)[gid] = PredValue(inst, model.trees, model.tree_info, gid,
                                    &feat_vecs[0], 0, ntree_limit) +
                          model.learner_model_param->base_score;
    }
  }

  void PredictLeaf(DMatrix* p_fmat, HostDeviceVector<bst_float>* out_preds,
                   const gbm::GBTreeModel& model, unsigned ntree_limit) const override {
    const int nthread = omp_get_max_threads();
    std::vector<RegTree::FVec> feat_vecs;
    const int num_feature = model.learner_model_param->num_feature;
    InitThreadTemp(nthread, num_feature, &feat_vecs);
    const MetaInfo& info = p_fmat->Info();
    // number of valid trees
    if (ntree_limit == 0 || ntree_limit > model.trees.size()) {
      ntree_limit = static_cast<unsigned>(model.trees.size());
    }
    std::vector<bst_float>& preds = out_preds->HostVector();
    preds.resize(info.num_row_ * ntree_limit);
    // start collecting the prediction
    for (const auto &batch : p_fmat->GetBatches<SparsePage>()) {
      // parallel over local batch
      auto page = batch.GetView();
      const auto nsize = static_cast<bst_omp_uint>(batch.Size());
      common::ParallelFor(nsize, [&](bst_omp_uint i) {
        const int tid = omp_get_thread_num();
        auto ridx = static_cast<size_t>(batch.base_rowid + i);
        RegTree::FVec &feats = feat_vecs[tid];
        if (feats.Size() == 0) {
          feats.Init(num_feature);
        }
        feats.Fill(page[i]);
        for (unsigned j = 0; j < ntree_limit; ++j) {
          int tid = model.trees[j]->GetLeafIndex(feats);
          preds[ridx * ntree_limit + j] = static_cast<bst_float>(tid);
        }
        feats.Drop(page[i]);
      });
    }
  }

  void PredictContribution(DMatrix* p_fmat, HostDeviceVector<float>* out_contribs,
                           const gbm::GBTreeModel& model, uint32_t ntree_limit,
                           std::vector<bst_float>* tree_weights,
                           bool approximate, int *group_indices, int num_feat_group,
                           int condition, unsigned condition_feature) const override {
    const int nthread = omp_get_max_threads();
    const int num_feature = model.learner_model_param->num_feature;
    std::vector<RegTree::FVec> feat_vecs;
    InitThreadTemp(nthread,  num_feature, &feat_vecs);
    const MetaInfo& info = p_fmat->Info();
    // number of valid trees
    if (ntree_limit == 0 || ntree_limit > model.trees.size()) {
      ntree_limit = static_cast<unsigned>(model.trees.size());
    }
    const int ngroup = model.learner_model_param->num_output_group;
    CHECK_NE(ngroup, 0);
    size_t const ncolumns = ((group_indices == nullptr) ? num_feature : num_feat_group) + 1;
    CHECK_NE(ncolumns, 0);
    // allocate space for (number of features + bias) times the number of rows
    std::vector<bst_float>& contribs = out_contribs->HostVector();
    contribs.resize(info.num_row_ * ncolumns * model.learner_model_param->num_output_group);
    // make sure contributions is zeroed, we could be reusing a previously
    // allocated one
    std::fill(contribs.begin(), contribs.end(), 0);
    // initialize tree node mean values
    common::ParallelFor(bst_omp_uint(ntree_limit), [&](bst_omp_uint i) {
      model.trees[i]->FillNodeMeanValues();
    });
    const std::vector<bst_float>& base_margin = info.base_margin_.HostVector();
    // start collecting the contributions
    for (const auto &batch : p_fmat->GetBatches<SparsePage>()) {
      auto page = batch.GetView();
      // parallel over local batch
      const auto nsize = static_cast<bst_omp_uint>(batch.Size());
      common::ParallelFor(nsize, [&](bst_omp_uint i) {
        auto row_idx = static_cast<size_t>(batch.base_rowid + i);
        RegTree::FVec &feats = feat_vecs[omp_get_thread_num()];
        if (feats.Size() == 0) {
          feats.Init(num_feature);
        }
        std::vector<bst_float> this_tree_contribs(ncolumns);
        // loop over all classes
        for (int gid = 0; gid < ngroup; ++gid) {
          bst_float* p_contribs = &contribs[(row_idx * ngroup + gid) * ncolumns];
          feats.Fill(page[i]);
          // calculate contributions
          for (unsigned j = 0; j < ntree_limit; ++j) {
            std::fill(this_tree_contribs.begin(), this_tree_contribs.end(), 0);
            if (model.tree_info[j] != gid) {
              continue;
            }
            if (!approximate) {
              model.trees[j]->CalculateContributions(feats, &this_tree_contribs[0],
                                                     group_indices, num_feat_group,
                                                     condition, condition_feature);
            } else {
              model.trees[j]->CalculateContributionsApprox(feats, &this_tree_contribs[0]);
            }
            for (size_t ci = 0 ; ci < ncolumns ; ++ci) {
                p_contribs[ci] += this_tree_contribs[ci] *
                    (tree_weights == nullptr ? 1 : (*tree_weights)[j]);
            }
          }
          feats.Drop(page[i]);
          // add base margin to BIAS
          if (base_margin.size() != 0) {
            p_contribs[ncolumns - 1] += base_margin[row_idx * ngroup + gid];
          } else {
            p_contribs[ncolumns - 1] += model.learner_model_param->base_score;
          }
        }
      });
    }
  }

  void PredictInteractionContributions(DMatrix* p_fmat, HostDeviceVector<bst_float>* out_contribs,
                                       const gbm::GBTreeModel& model, unsigned ntree_limit,
                                       std::vector<bst_float>* tree_weights, bool approximate,
                                       int *group_indices, int num_feat_group) const override {
    const MetaInfo& info = p_fmat->Info();
    const int ngroup = model.learner_model_param->num_output_group;
    size_t const ncolumns = (group_indices == nullptr) ?
                        model.learner_model_param->num_feature : num_feat_group;
    const unsigned row_chunk = ngroup * (ncolumns + 1) * (ncolumns + 1);
    const unsigned mrow_chunk = (ncolumns + 1) * (ncolumns + 1);
    const unsigned crow_chunk = ngroup * (ncolumns + 1);

    // allocate space for (number of features^2) times the number of rows and tmp off/on contribs
    std::vector<bst_float>& contribs = out_contribs->HostVector();
    contribs.resize(info.num_row_ * ngroup * (ncolumns + 1) * (ncolumns + 1));
    HostDeviceVector<bst_float> contribs_off_hdv(info.num_row_ * ngroup * (ncolumns + 1));
    auto &contribs_off = contribs_off_hdv.HostVector();
    HostDeviceVector<bst_float> contribs_on_hdv(info.num_row_ * ngroup * (ncolumns + 1));
    auto &contribs_on = contribs_on_hdv.HostVector();
    HostDeviceVector<bst_float> contribs_diag_hdv(info.num_row_ * ngroup * (ncolumns + 1));
    auto &contribs_diag = contribs_diag_hdv.HostVector();

    // Compute the difference in effects when conditioning on each of the features on and off
    // see: Axiomatic characterizations of probabilistic and
    //      cardinal-probabilistic interaction indices
    PredictContribution(p_fmat, &contribs_diag_hdv, model, ntree_limit,
                        tree_weights, approximate, group_indices, num_feat_group, 0, 0);
    for (size_t i = 0; i < ncolumns + 1; ++i) {
      PredictContribution(p_fmat, &contribs_off_hdv, model, ntree_limit,
                          tree_weights, approximate, group_indices, num_feat_group, -1, i);
      PredictContribution(p_fmat, &contribs_on_hdv, model, ntree_limit,
                          tree_weights, approximate, group_indices, num_feat_group, 1, i);

      for (size_t j = 0; j < info.num_row_; ++j) {
        for (int l = 0; l < ngroup; ++l) {
          const unsigned o_offset = j * row_chunk + l * mrow_chunk + i * (ncolumns + 1);
          const unsigned c_offset = j * crow_chunk + l * (ncolumns + 1);
          contribs[o_offset + i] = 0;
          for (size_t k = 0; k < ncolumns + 1; ++k) {
            // fill in the diagonal with additive effects, and off-diagonal with the interactions
            if (k == i) {
              contribs[o_offset + i] += contribs_diag[c_offset + k];
            } else {
              contribs[o_offset + k] = (contribs_on[c_offset + k] - contribs_off[c_offset + k])/2.0;
              contribs[o_offset + i] -= contribs[o_offset + k];
            }
          }
        }
      }
    }
  }

 private:
  static size_t constexpr kBlockOfRowsSize = 64;
};

XGBOOST_REGISTER_PREDICTOR(CPUPredictor, "cpu_predictor")
.describe("Make predictions using CPU.")
.set_body([](GenericParameter const* generic_param) {
            return new CPUPredictor(generic_param);
          });
}  // namespace predictor
}  // namespace xgboost
