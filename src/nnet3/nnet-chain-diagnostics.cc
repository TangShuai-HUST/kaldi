// nnet3/nnet-chain-diagnostics.cc

// Copyright      2015    Johns Hopkins University (author: Daniel Povey)

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "nnet3/nnet-chain-diagnostics.h"
#include "nnet3/nnet-utils.h"

namespace kaldi {
namespace nnet3 {

NnetChainComputeProb::NnetChainComputeProb(
    const NnetComputeProbOptions &nnet_config,
    const chain::ChainTrainingOptions &chain_config,
    const fst::StdVectorFst &den_fst,
    const Nnet &nnet):
    nnet_config_(nnet_config),
    chain_config_(chain_config),
    den_graph_(den_fst, nnet.OutputDim("output")),
    nnet_(nnet),
    compiler_(nnet, nnet_config_.optimize_config, nnet_config_.compiler_config),
    deriv_nnet_owned_(true),
    deriv_nnet_(NULL),
    num_minibatches_processed_(0) {
  if (nnet_config_.compute_deriv) {
    deriv_nnet_ = new Nnet(nnet_);
    ScaleNnet(0.0, deriv_nnet_);
    SetNnetAsGradient(deriv_nnet_); // force simple update
  } else if (nnet_config_.store_component_stats) {
    KALDI_ERR << "If you set store_component_stats == true and "
              << "compute_deriv == false, use the other constructor.";
  }

  if (chain_config.use_smbr_objective &&
      (chain_config.exclude_silence || chain_config.one_silence_class)) {
    if (chain_config.silence_pdfs_str.empty()) {
      KALDI_ERR << "--silence-pdfs is required if --exclude-silence or "
                << "--one-silence-class is true.";
    }

    std::vector<std::string> silence_pdfs;
    SplitStringToVector(chain_config.silence_pdfs_str, ":,", false, 
                        &silence_pdfs);

    int32 num_pdfs = nnet.OutputDim("output");
    std::vector<int32> indices(num_pdfs, -1);

    if (chain_config.exclude_silence) {
      for (size_t i = 0; i < num_pdfs; i++) {
        indices[i] = i;
      }

      for (std::vector<std::string>::iterator it = silence_pdfs.begin();
           it != silence_pdfs.end(); ++it) {
        int32 pdf = std::atoi(it->c_str());
        if (pdf > num_pdfs) 
          KALDI_ERR << "Invalid pdf " << pdf << " in silence-pdfs "
                    << chain_config.silence_pdfs_str;
        indices[pdf] = -1;
      }
    } else {
      for (std::vector<std::string>::iterator it = silence_pdfs.begin();
           it != silence_pdfs.end(); ++it) {
        int32 pdf = std::atoi(it->c_str());
        if (pdf > num_pdfs) 
          KALDI_ERR << "Invalid pdf " << pdf << " in silence-pdfs "
                    << chain_config.silence_pdfs_str;
        indices[pdf] = pdf;
      }
    }

    sil_indices_.Resize(num_pdfs);
    sil_indices_.CopyFromVec(indices);
  }
  
  if (!nnet_config.objective_scales_str.empty()) {
    std::vector<std::string> objectives_for_outputs;
    SplitStringToVector(nnet_config.objective_scales_str, ",", false,
                        &objectives_for_outputs);
    std::vector<std::string>::const_iterator it = objectives_for_outputs.begin();
    for (; it != objectives_for_outputs.end(); ++it) {
      std::vector<std::string> this_output_objective;
      SplitStringToVector(*it, ":", false,
                          &this_output_objective);

      BaseFloat scale;
      ConvertStringToReal(this_output_objective[1], &scale);
      objective_scales_.insert(
          std::make_pair(this_output_objective[0], scale));
    }
  }
}


NnetChainComputeProb::NnetChainComputeProb(
    const NnetComputeProbOptions &nnet_config,
    const chain::ChainTrainingOptions &chain_config,
    const fst::StdVectorFst &den_fst,
    Nnet *nnet):
    nnet_config_(nnet_config),
    chain_config_(chain_config),
    den_graph_(den_fst, nnet->OutputDim("output")),
    nnet_(*nnet),
    compiler_(*nnet, nnet_config_.optimize_config, nnet_config_.compiler_config),
    deriv_nnet_owned_(false),
    deriv_nnet_(nnet),
    num_minibatches_processed_(0) {
  KALDI_ASSERT(den_graph_.NumPdfs() > 0);
  KALDI_ASSERT(nnet_config.store_component_stats && !nnet_config.compute_deriv);

  if (!chain_config.silence_pdfs_str.empty()) {
    std::vector<std::string> silence_pdfs;
    SplitStringToVector(chain_config.silence_pdfs_str, ":,", false, 
                        &silence_pdfs);

    int32 num_pdfs = nnet->OutputDim("output");
    std::vector<int32> indices(num_pdfs);
    for (size_t i = 0; i < num_pdfs; i++) {
      indices[i] = i;
    }
    
    for (std::vector<std::string>::iterator it = silence_pdfs.begin();
         it != silence_pdfs.end(); ++it) {
      int32 pdf = std::atoi(it->c_str());
      if (pdf > num_pdfs) 
        KALDI_ERR << "Invalid pdf " << pdf << " in silence-pdfs "
                  << chain_config.silence_pdfs_str;
      indices[pdf] = -1;
    }

    sil_indices_.Resize(num_pdfs);
    sil_indices_.CopyFromVec(indices);
  }
}


const Nnet &NnetChainComputeProb::GetDeriv() const {
  if (!nnet_config_.compute_deriv)
    KALDI_ERR << "GetDeriv() called when no derivatives were requested.";
  return *deriv_nnet_;
}

NnetChainComputeProb::~NnetChainComputeProb() {
  if (deriv_nnet_owned_)
    delete deriv_nnet_;  // delete does nothing if pointer is NULL.
}

void NnetChainComputeProb::Reset() {
  num_minibatches_processed_ = 0;
  objf_info_.clear();
  if (deriv_nnet_) {
    ScaleNnet(0.0, deriv_nnet_);
    SetNnetAsGradient(deriv_nnet_);
  }
}

void NnetChainComputeProb::Compute(const NnetChainExample &chain_eg) {
  bool need_model_derivative = nnet_config_.compute_deriv,
      store_component_stats = nnet_config_.store_component_stats;
  ComputationRequest request;
  // if the options specify cross-entropy regularization, we'll be computing
  // this objective (not interpolated with the regular objective-- we give it a
  // separate name), but currently we won't make it contribute to the
  // derivative-- we just compute the derivative of the regular output.
  // This is because in the place where we use the derivative (the
  // model-combination code) we decided to keep it simple and just use the
  // regular objective.
  bool use_xent_regularization = (chain_config_.xent_regularize != 0.0),
      use_xent_derivative = false;
  GetChainComputationRequest(nnet_, chain_eg, need_model_derivative,
                             store_component_stats, use_xent_regularization,
                             use_xent_derivative, &request);
  const NnetComputation *computation = compiler_.Compile(request);
  NnetComputer computer(nnet_config_.compute_config, *computation,
                        nnet_, deriv_nnet_);
  // give the inputs to the computer object.
  computer.AcceptInputs(nnet_, chain_eg.inputs);
  computer.Run();
  this->ProcessOutputs(chain_eg, &computer);
  if (nnet_config_.compute_deriv)
    computer.Run();
}

void NnetChainComputeProb::ProcessOutputs(const NnetChainExample &eg,
                                         NnetComputer *computer) {
  // There will normally be just one output here, named 'output',
  // but the code is more general than this.
  std::vector<NnetChainSupervision>::const_iterator iter = eg.outputs.begin(),
      end = eg.outputs.end();
  for (; iter != end; ++iter) {
    const NnetChainSupervision &sup = *iter;
    int32 node_index = nnet_.GetNodeIndex(sup.name);
    if (node_index < 0 ||
        !nnet_.IsOutputNode(node_index))
      KALDI_ERR << "Network has no output named " << sup.name;

    const CuMatrixBase<BaseFloat> &nnet_output = computer->GetOutput(sup.name);
    bool use_xent = (chain_config_.xent_regularize != 0.0);
    std::string xent_name = sup.name + "-xent";  // typically "output-xent".
    CuMatrix<BaseFloat> nnet_output_deriv, xent_deriv;
    if (nnet_config_.compute_deriv)
      nnet_output_deriv.Resize(nnet_output.NumRows(), nnet_output.NumCols(),
                               kUndefined);
    if (use_xent)
      xent_deriv.Resize(nnet_output.NumRows(), nnet_output.NumCols(),
                        kUndefined);

    BaseFloat tot_like, tot_mmi_objf, tot_l2_term, tot_weight;

    if (sup.supervision.numerator_post_targets.NumRows() > 0) {
      ComputeKLObjfAndDeriv(chain_config_, den_graph_,
                            sup.supervision, nnet_output,
                            &tot_like, &tot_l2_term, &tot_weight,
                            (nnet_config_.compute_deriv ? &nnet_output_deriv :
                             NULL), (use_xent ? &xent_deriv : NULL));
    } else {
      if (chain_config_.use_smbr_objective)
        ComputeChainSmbrObjfAndDeriv(
            chain_config_, den_graph_,
            sup.supervision, nnet_output,
            &tot_like, &tot_mmi_objf, &tot_l2_term, &tot_weight,
            (nnet_config_.compute_deriv ? &nnet_output_deriv :
             NULL), (use_xent ? &xent_deriv : NULL),
            sil_indices_.Dim() ? &sil_indices_ : NULL);
      else
        ComputeChainObjfAndDeriv(chain_config_, den_graph_,
                                 sup.supervision, nnet_output,
                                 &tot_like, &tot_l2_term, &tot_weight,
                                 (nnet_config_.compute_deriv ? &nnet_output_deriv :
                                  NULL), (use_xent ? &xent_deriv : NULL));
    }

    BaseFloat objf_scale = 1.0;
    {
      unordered_map<std::string, BaseFloat, StringHasher>::iterator it =
        objective_scales_.find(sup.name);

      if (it != objective_scales_.end()) {
        objf_scale = it->second;
        tot_like *= it->second;
        tot_l2_term *= it->second;
        tot_mmi_objf *= it->second;
        tot_weight *= it->second;
        if (nnet_config_.compute_deriv) 
          nnet_output_deriv.Scale(it->second);
      }
    }

    // note: in this context we don't want to apply 'sup.deriv_weights' because
    // this code is used only in combination, where it's part of an L-BFGS
    // optimization algorithm, and in that case if there is a mismatch between
    // the computed objective function and the derivatives, it may cause errors
    // in the optimization procedure such as early termination.  (line search
    // and conjugate gradient descent both rely on the derivatives being
    // accurate, and don't fail gracefully if the derivatives are not accurate).

    std::vector<double> aux_objfs;
    aux_objfs.push_back(tot_l2_term);
    if (chain_config_.use_smbr_objective)
      aux_objfs.push_back(tot_mmi_objf);

    {
      unordered_map<std::string, ChainObjectiveInfo, StringHasher>::iterator it 
        = objf_info_.find(sup.name);

      if (it == objf_info_.end()) {
        BaseFloat this_objf_scale = objf_scale;
        std::vector<BaseFloat> aux_objf_scales(1, objf_scale); // for l2 term
        if (chain_config_.use_smbr_objective) {
          this_objf_scale *= chain_config_.smbr_factor;
          aux_objf_scales.push_back(objf_scale * chain_config_.mmi_factor);
        }

        ChainObjectiveInfo totals(this_objf_scale, aux_objf_scales);
        it = objf_info_.insert(it, std::make_pair(sup.name, totals));
      }

      it->second.tot_weight += tot_weight;
      it->second.tot_like += tot_like;
      it->second.tot_aux_objfs.Add(aux_objfs);
    }

    if (nnet_config_.compute_deriv)
      computer->AcceptInput(sup.name, &nnet_output_deriv);

    if (use_xent) {
      ChainObjectiveInfo &xent_totals = objf_info_[xent_name];
      // this block computes the cross-entropy objective.
      const CuMatrixBase<BaseFloat> &xent_output = computer->GetOutput(
          xent_name);
      // at this point, xent_deriv is posteriors derived from the numerator
      // computation.  note, xent_deriv has a factor of '.supervision.weight',
      // but so does tot_weight.
      BaseFloat xent_objf = TraceMatMat(xent_output, xent_deriv, kTrans);
      unordered_map<std::string, BaseFloat, StringHasher>::iterator it =
        objective_scales_.find(xent_name);

      if (it != objective_scales_.end()) {
        xent_objf *= it->second;
        xent_deriv.Scale(it->second);
      }

      xent_totals.tot_weight += tot_weight;
      xent_totals.tot_like += xent_objf;
    }
    num_minibatches_processed_++;
  }
}

/*
void NnetChainComputeProb::Compute(const NnetExample &eg) {
  bool need_model_derivative = nnet_config_.compute_deriv,
      store_component_stats = nnet_config_.store_component_stats;
  ComputationRequest request;
  // if the options specify cross-entropy regularization, we'll be computing
  // this objective (not interpolated with the regular objective-- we give it a
  // separate name), but currently we won't make it contribute to the
  // derivative-- we just compute the derivative of the regular output.
  // This is because in the place where we use the derivative (the
  // model-combination code) we decided to keep it simple and just use the
  // regular objective.
  bool use_xent_regularization = (chain_config_.xent_regularize != 0.0),
      use_xent_derivative = false;
  GetComputationRequest(nnet_, eg, need_model_derivative,
                        store_component_stats, &request,
                        use_xent_regularization, use_xent_derivative);
  const NnetComputation *computation = compiler_.Compile(request);
  NnetComputer computer(nnet_config_.compute_config, *computation,
                        nnet_, deriv_nnet_);
  // give the inputs to the computer object.
  computer.AcceptInputs(nnet_, eg.io);
  computer.Run();
  this->ProcessOutputs(eg, &computer);
  if (nnet_config_.compute_deriv)
    computer.Run();
}

void NnetChainComputeProb::ProcessOutputs(const NnetExample &eg,
                                          NnetComputer *computer) {
  // There will normally be just one output here, named 'output',
  // but the code is more general than this.
  std::vector<NnetIo>::const_iterator iter = eg.io.begin(),
      end = eg.io.end();
  for (; iter != end; ++iter) {
    const NnetIo &io = *iter;
    int32 node_index = nnet_.GetNodeIndex(io.name);
    if (!nnet_.IsOutputNode(node_index)) continue;

    const CuMatrixBase<BaseFloat> &nnet_output = computer->GetOutput(io.name);
    bool use_xent = (chain_config_.xent_regularize != 0.0);
    std::string xent_name = io.name + "-xent";  // typically "output-xent".
    CuMatrix<BaseFloat> nnet_output_deriv, xent_deriv;
    if (nnet_config_.compute_deriv)
      nnet_output_deriv.Resize(nnet_output.NumRows(), nnet_output.NumCols(),
                               kUndefined);
    if (use_xent)
      xent_deriv.Resize(nnet_output.NumRows(), nnet_output.NumCols(),
                        kUndefined);

    BaseFloat tot_like, tot_l2_term, tot_weight;

    int32 num_sequences = NumSequencesInChainEg(io.indexes);
    KALDI_ASSERT(io.features.NumRows() % num_sequences == 0);
    int32 frames_per_sequence = io.features.NumRows() / num_sequences;
    ComputeKLObjfAndDeriv(chain_config_, den_graph_,
                          io.features, 1.0, nnet_output,
                          num_sequences, frames_per_sequence,
                          &tot_like, &tot_l2_term, &tot_weight,
                          (nnet_config_.compute_deriv ? &nnet_output_deriv :
                           NULL), (use_xent ? &xent_deriv : NULL));

    BaseFloat objf_scale = 1.0;
    {
      unordered_map<std::string, BaseFloat, StringHasher>::iterator it =
        objective_scales_.find(io.name);

      if (it != objective_scales_.end()) {
        objf_scale = it->second;
        tot_like *= it->second;
        tot_l2_term *= it->second;
        tot_weight *= it->second;
        if (nnet_config_.compute_deriv)
          nnet_output_deriv.Scale(it->second);
      }
    }

    // note: in this context we don't want to apply 'io.deriv_weights' because
    // this code is used only in combination, where it's part of an L-BFGS
    // optimization algorithm, and in that case if there is a mismatch between
    // the computed objective function and the derivatives, it may cause errors
    // in the optimization procedure such as early termination.  (line search
    // and conjugate gradient descent both rely on the derivatives being
    // accurate, and don't fail gracefully if the derivatives are not accurate).

    std::vector<double> aux_objfs;
    aux_objfs.push_back(tot_l2_term);

    {
      unordered_map<std::string, ChainObjectiveInfo, StringHasher>::iterator it 
        = objf_info_.find(io.name);

      if (it == objf_info_.end()) {
        BaseFloat this_objf_scale = objf_scale;
        std::vector<BaseFloat> aux_objf_scales(1, objf_scale); // for l2 term

        ChainObjectiveInfo totals(this_objf_scale, aux_objf_scales);
        it = objf_info_.insert(it, std::make_pair(io.name, totals));
      }

      it->second.tot_weight += tot_weight;
      it->second.tot_like += tot_like;
      it->second.tot_aux_objfs.Add(aux_objfs);
    }

    if (nnet_config_.compute_deriv)
      computer->AcceptInput(io.name, &nnet_output_deriv);

    if (use_xent) {
      ChainObjectiveInfo &xent_totals = objf_info_[xent_name];
      // this block computes the cross-entropy objective.
      const CuMatrixBase<BaseFloat> &xent_output = computer->GetOutput(
          xent_name);
      // at this point, xent_deriv is posteriors derived from the numerator
      // computation.  note, xent_deriv has a factor of '.supervision.weight',
      // but so does tot_weight.
      BaseFloat xent_objf = TraceMatMat(xent_output, xent_deriv, kTrans);
      unordered_map<std::string, BaseFloat, StringHasher>::iterator it =
        objective_scales_.find(xent_name);

      if (it != objective_scales_.end()) {
        xent_objf *= it->second;
        xent_deriv.Scale(it->second);
      }

      xent_totals.tot_weight += tot_weight;
      xent_totals.tot_like += xent_objf;
    }
    num_minibatches_processed_++;
  }
}
*/

bool NnetChainComputeProb::PrintTotalStats() const {
  bool ans = false;
  unordered_map<std::string, ChainObjectiveInfo, StringHasher>::const_iterator
      iter, end;
  iter = objf_info_.begin();
  end = objf_info_.end();
  for (; iter != end; ++iter) {
    const std::string &name = iter->first;
    int32 node_index = nnet_.GetNodeIndex(name);
    KALDI_ASSERT(node_index >= 0);
    const ChainObjectiveInfo &info = iter->second;
    BaseFloat like = (info.tot_like / info.tot_weight);

    ObjectiveValues aux_objfs(info.tot_aux_objfs);
    aux_objfs.InvScale(info.tot_weight);
    BaseFloat tot_objf = like + aux_objfs.Sum();

    // Remove scales for the purpose of printing
    if (info.objf_scale != 0.0) like /= info.objf_scale;
    if (info.aux_objf_scales.size() > 0)
      aux_objfs.InvScale(info.aux_objf_scales);

    if (info.tot_aux_objfs.IsZero()) {
      KALDI_LOG << "Overall log-probability for '"
                << name << "' is "
                << like << " per frame"
                << ", over " << info.tot_weight << " frames.";
    } else {
      KALDI_LOG << "Overall log-probability for '"
                << name << "' is "
                << like << " + " << aux_objfs.Str() 
                << " = " << tot_objf << " per frame"
                << ", over " << info.tot_weight << " frames.";
    }
    if (info.tot_weight > 0)
      ans = true;
  }
  return ans;
}


std::pair<BaseFloat, BaseFloat> NnetChainComputeProb::GetTotalObjective() const {
  unordered_map<std::string, ChainObjectiveInfo, StringHasher>::const_iterator
      iter, end;
  iter = objf_info_.begin();
  end = objf_info_.end();
  BaseFloat tot_objf = 0.0, tot_weight = 0.0;
  for (; iter != end; ++iter) {
    const std::string &name = iter->first;
    int32 node_index = nnet_.GetNodeIndex(name);
    KALDI_ASSERT(node_index >= 0);
    const ChainObjectiveInfo &info = iter->second;
    BaseFloat like = (info.tot_like / info.tot_weight);
    ObjectiveValues aux_objfs(info.tot_aux_objfs);
    aux_objfs.Scale(info.tot_weight);
    tot_objf += like + aux_objfs.Sum();
    tot_weight += info.tot_weight;
  }
  return std::make_pair(tot_objf, tot_weight);
}


const ChainObjectiveInfo* NnetChainComputeProb::GetObjective(
    const std::string &output_name) const {
  unordered_map<std::string, ChainObjectiveInfo, StringHasher>::const_iterator
      iter = objf_info_.find(output_name);
  if (iter != objf_info_.end())
    return &(iter->second);
  else
    return NULL;
}

static bool HasXentOutputs(const Nnet &nnet) {
  const std::vector<std::string> node_names = nnet.GetNodeNames();
  for (std::vector<std::string>::const_iterator it = node_names.begin();
        it != node_names.end(); ++it) {
    int32 node_index = nnet.GetNodeIndex(*it);
    if (nnet.IsOutputNode(node_index) && 
        it->find("-xent") != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RecomputeStats(const std::vector<NnetChainExample> &egs,
                    const chain::ChainTrainingOptions &chain_config_in,
                    const fst::StdVectorFst &den_fst,
                    Nnet *nnet) {
  KALDI_LOG << "Recomputing stats on nnet (affects batch-norm)";
  chain::ChainTrainingOptions chain_config(chain_config_in);
  if (HasXentOutputs(*nnet) &&
      chain_config.xent_regularize == 0) {
    // this forces it to compute the output for xent outputs, 
    // usually 'output-xent', which
    // means that we'll be computing batch-norm stats for any
    // components in that branch that have batch-norm.
    chain_config.xent_regularize = 0.1;
  }

  ZeroComponentStats(nnet);
  NnetComputeProbOptions nnet_config;
  nnet_config.store_component_stats = true;
  NnetChainComputeProb prob_computer(nnet_config, chain_config, den_fst, nnet);
  for (size_t i = 0; i < egs.size(); i++)
    prob_computer.Compute(egs[i]);
  prob_computer.PrintTotalStats();
  KALDI_LOG << "Done recomputing stats.";
}

/*
void RecomputeStats(const std::vector<NnetExample> &egs,
                    const chain::ChainTrainingOptions &chain_config_in,
                    const fst::StdVectorFst &den_fst,
                    Nnet *nnet) {
  KALDI_LOG << "Recomputing stats on nnet (affects batch-norm)";
  chain::ChainTrainingOptions chain_config(chain_config_in);
  if (HasXentOutputs(*nnet) &&
      chain_config.xent_regularize == 0) {
    // this forces it to compute the output for xent outputs, 
    // usually 'output-xent', which
    // means that we'll be computing batch-norm stats for any
    // components in that branch that have batch-norm.
    chain_config.xent_regularize = 0.1;
  }

  ZeroComponentStats(nnet);
  NnetComputeProbOptions nnet_config;
  nnet_config.store_component_stats = true;
  NnetChainComputeProb prob_computer(nnet_config, chain_config, den_fst, nnet);
  for (size_t i = 0; i < egs.size(); i++)
    prob_computer.Compute(egs[i]);
  prob_computer.PrintTotalStats();
  KALDI_LOG << "Done recomputing stats.";
}
*/


} // namespace nnet3
} // namespace kaldi
