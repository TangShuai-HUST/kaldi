// chain/chain-training.cc

// Copyright      2015   Johns Hopkins University (author: Daniel Povey)
//                2018   Hossein Hadian

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

#include "chain/chain-training.h"
#include "chain/chain-kernels-ansi.h"
#include "chain/chain-numerator.h"
#include "chain/chain-generic-numerator.h"
#include "chain/chain-denominator.h"
#include "chain/chain-denominator-smbr.h"
#include "hmm/posterior.h"

namespace kaldi {
namespace chain {


void ComputeChainObjfAndDerivE2e(const ChainTrainingOptions &opts,
                                 const DenominatorGraph &den_graph,
                                 const Supervision &supervision,
                                 const CuMatrixBase<BaseFloat> &nnet_output,
                                 BaseFloat *objf,
                                 BaseFloat *l2_term,
                                 BaseFloat *weight,
                                 CuMatrixBase<BaseFloat> *nnet_output_deriv,
                                 CuMatrix<BaseFloat> *xent_output_deriv) {
  BaseFloat num_logprob_weighted, den_logprob_weighted;
  bool denominator_ok = true;
  bool numerator_ok = true;
  *weight = supervision.weight * supervision.num_sequences *
      supervision.frames_per_sequence;

  if (nnet_output_deriv != NULL)
    nnet_output_deriv->SetZero();

  { // Doing the denominator first helps to reduce the maximum
    // memory use, as we can set 'xent_deriv' to nonempty after
    // we've freed the memory in this object.
    DenominatorComputation denominator(opts, den_graph,
                                       supervision.num_sequences,
                                       nnet_output);

    den_logprob_weighted = supervision.weight * denominator.Forward();
    if (nnet_output_deriv)
      denominator_ok = denominator.Backward(-supervision.weight,
                                nnet_output_deriv);
  }

  if (xent_output_deriv != NULL) {
    // the reason for kStrideEqualNumCols is so that we can share the memory
    // block with the memory that was used for exp_nnet_output_transposed_ from
    // chain-denominator.cc, which has just been freed; it also uses the
    // kStrideEqualNumCols arg (its shape is the transpose of this matrix's
    // shape).
    xent_output_deriv->Resize(nnet_output.NumRows(), nnet_output.NumCols(),
                              kSetZero, kStrideEqualNumCols);
  }


  {
    GenericNumeratorComputation numerator(supervision, nnet_output);
    // note: supervision.weight is included as a factor in the derivative from
    // the numerator object, as well as the returned logprob.
    num_logprob_weighted = numerator.Forward();
    KALDI_VLOG(2) << "Numerator logprob per frame: "
                  << num_logprob_weighted / (*weight);
    numerator_ok = (num_logprob_weighted - num_logprob_weighted == 0);
    if (!numerator_ok)
      KALDI_LOG << "Numerator forward failed.";

    if (xent_output_deriv && numerator_ok) {
      numerator_ok = numerator.Backward(xent_output_deriv);
      if (!numerator_ok)
        KALDI_LOG << "Numerator backward failed.";
      if (nnet_output_deriv)
        nnet_output_deriv->AddMat(1.0, *xent_output_deriv);
    } else if (nnet_output_deriv && numerator_ok) {
      numerator_ok = numerator.Backward(nnet_output_deriv);
      if (!numerator_ok)
        KALDI_LOG << "Numerator backward failed.";
    }
  }

  *objf = num_logprob_weighted - den_logprob_weighted;
  if (!((*objf) - (*objf) == 0) || !denominator_ok || !numerator_ok) {
    // inf or NaN detected, or denominator computation returned false.
    if (nnet_output_deriv)
      nnet_output_deriv->SetZero();
    if (xent_output_deriv)
      xent_output_deriv->SetZero();
    BaseFloat default_objf = -10;
    KALDI_WARN << "Objective function is " << (*objf)
               << " and denominator computation (if done) returned "
               << std::boolalpha << denominator_ok
               << " and numerator computation returned "
               << std::boolalpha << numerator_ok
               << ", setting objective function to " << default_objf
               << " per frame.";
    *objf  = default_objf * *weight;
  }

  // This code helps us see how big the derivatives are, on average,
  // for different frames of the sequences.  As expected, they are
  // smaller towards the edges of the sequences (due to the penalization
  // of 'incorrect' pdf-ids.
  if (GetVerboseLevel() >= 1 && nnet_output_deriv != NULL && RandInt(0, 10) == 0) {
    int32 tot_frames = nnet_output_deriv->NumRows(),
 frames_per_sequence = supervision.frames_per_sequence,
       num_sequences = supervision.num_sequences;
    CuVector<BaseFloat> row_products(tot_frames);
    row_products.AddDiagMat2(1.0, *nnet_output_deriv, kNoTrans, 0.0);
    Vector<BaseFloat> row_products_cpu(row_products);
    Vector<BaseFloat> row_products_per_frame(frames_per_sequence);
    for (int32 i = 0; i < tot_frames; i++)
      row_products_per_frame(i / num_sequences) += row_products_cpu(i);
    KALDI_LOG << "Derivs per frame are " << row_products_per_frame;
  }

  *l2_term = 0.0;
  if (opts.l2_regularize != 0.0 && numerator_ok) {  // we should have some derivs to include a L2 term
    // compute the l2 penalty term and its derivative
    BaseFloat scale = supervision.weight * opts.l2_regularize;
    *l2_term = -0.5 * scale * TraceMatMat(nnet_output, nnet_output, kTrans);
    if (nnet_output_deriv)
      nnet_output_deriv->AddMat(-1.0 * scale, nnet_output);
  }
}

void ComputeKLObjfAndDeriv(const ChainTrainingOptions &opts,
                           const DenominatorGraph &den_graph,
                           const Supervision &supervision,
                           const CuMatrixBase<BaseFloat> &nnet_output,
                           BaseFloat *objf,
                           BaseFloat *l2_term,
                           BaseFloat *weight,
                           CuMatrixBase<BaseFloat> *nnet_output_deriv,
                           CuMatrix<BaseFloat> *xent_output_deriv) {
  KALDI_ASSERT(supervision.numerator_post_targets.NumRows() > 0);
  KALDI_ASSERT(nnet_output.NumRows() == supervision.num_sequences * supervision.frames_per_sequence);
  KALDI_ASSERT(supervision.numerator_post_targets.NumRows() == nnet_output.NumRows());

  BaseFloat den_logprob_weighted;
  bool ok = true;
  if (nnet_output_deriv != NULL)
    nnet_output_deriv->SetZero();

  { // Doing the denominator first helps to reduce the maximum
    // memory use, as we can set 'xent_deriv' to nonempty after
    // we've freed the memory in this object.
    DenominatorComputation denominator(opts, den_graph,
                                       supervision.num_sequences,
                                       nnet_output);

    den_logprob_weighted = supervision.weight * denominator.Forward();
    if (nnet_output_deriv)
      ok = denominator.Backward(-supervision.weight,
                                nnet_output_deriv);
  }

  if (xent_output_deriv != NULL) {
    // the reason for kStrideEqualNumCols is so that we can share the memory
    // block with the memory that was used for exp_nnet_output_transposed_ from
    // chain-denominator.cc, which has just been freed; it also uses the
    // kStrideEqualNumCols arg (its shape is the transpose of this matrix's
    // shape).
    xent_output_deriv->Resize(nnet_output.NumRows(), nnet_output.NumCols(),
                              kSetZero, kStrideEqualNumCols);
    supervision.numerator_post_targets.CopyToMat(xent_output_deriv);
    xent_output_deriv->Scale(supervision.weight);
    if (nnet_output_deriv)
      nnet_output_deriv->AddMat(1.0, *xent_output_deriv);
  } else if (nnet_output_deriv) {
    CuMatrix<BaseFloat> numerator_post(nnet_output.NumRows(), nnet_output.NumCols());
    supervision.numerator_post_targets.CopyToMat(&numerator_post);
    nnet_output_deriv->AddMat(supervision.weight, numerator_post);
  }

  *objf = -den_logprob_weighted;
  *weight = supervision.weight * supervision.num_sequences * supervision.frames_per_sequence;
  if (!((*objf) - (*objf) == 0) || !ok) {
    // inf or NaN detected, or denominator computation returned false.
    if (nnet_output_deriv)
      nnet_output_deriv->SetZero();
    if (xent_output_deriv)
      xent_output_deriv->SetZero();
    BaseFloat default_objf = -10;
    KALDI_WARN << "Objective function is " << (*objf)
               << " and denominator computation (if done) returned "
               << std::boolalpha << ok
               << ", setting objective function to " << default_objf
               << " per frame.";
    *objf  = default_objf * *weight;
  }

  // This code helps us see how big the derivatives are, on average,
  // for different frames of the sequences.  As expected, they are
  // smaller towards the edges of the sequences (due to the penalization
  // of 'incorrect' pdf-ids.
  if (GetVerboseLevel() >= 1 && nnet_output_deriv != NULL && RandInt(0, 10) == 0) {
    int32 tot_frames = nnet_output_deriv->NumRows();
    CuVector<BaseFloat> row_products(tot_frames);
    row_products.AddDiagMat2(1.0, *nnet_output_deriv, kNoTrans, 0.0);
    Vector<BaseFloat> row_products_cpu(row_products);
    Vector<BaseFloat> row_products_per_frame(supervision.frames_per_sequence);
    for (int32 i = 0; i < tot_frames; i++)
      row_products_per_frame(i / supervision.num_sequences) += row_products_cpu(i);
    KALDI_LOG << "Derivs per frame are " << row_products_per_frame;
  }

  if (opts.l2_regularize == 0.0) {
    *l2_term = 0.0;
  } else {
    // compute the l2 penalty term and its derivative
    BaseFloat scale = supervision.weight * opts.l2_regularize;
    *l2_term = -0.5 * scale * TraceMatMat(nnet_output, nnet_output, kTrans);
    if (nnet_output_deriv)
      nnet_output_deriv->AddMat(-1.0 * scale, nnet_output);
  }
}


void ComputeChainObjfAndDeriv(const ChainTrainingOptions &opts,
                              const DenominatorGraph &den_graph,
                              const Supervision &supervision,
                              const CuMatrixBase<BaseFloat> &nnet_output,
                              BaseFloat *objf,
                              BaseFloat *l2_term,
                              BaseFloat *weight,
                              CuMatrixBase<BaseFloat> *nnet_output_deriv,
                              CuMatrix<BaseFloat> *xent_output_deriv) {
  if (supervision.e2e) {
    ComputeChainObjfAndDerivE2e(opts, den_graph, supervision,
                                nnet_output, objf, l2_term,
                                weight, nnet_output_deriv, xent_output_deriv);
    return;
  }

  BaseFloat num_logprob_weighted, den_logprob_weighted;
  bool ok = true;
  if (nnet_output_deriv != NULL)
    nnet_output_deriv->SetZero();

  { // Doing the denominator first helps to reduce the maximum
    // memory use, as we can set 'xent_deriv' to nonempty after
    // we've freed the memory in this object.
    DenominatorComputation denominator(opts, den_graph,
                                       supervision.num_sequences,
                                       nnet_output);

    den_logprob_weighted = supervision.weight * denominator.Forward();
    if (nnet_output_deriv)
      ok = denominator.Backward(-supervision.weight,
                                nnet_output_deriv);
  }

  if (xent_output_deriv != NULL) {
    // the reason for kStrideEqualNumCols is so that we can share the memory
    // block with the memory that was used for exp_nnet_output_transposed_ from
    // chain-denominator.cc, which has just been freed; it also uses the
    // kStrideEqualNumCols arg (its shape is the transpose of this matrix's
    // shape).
    xent_output_deriv->Resize(nnet_output.NumRows(), nnet_output.NumCols(),
                              kSetZero, kStrideEqualNumCols);
  }


  {
    NumeratorComputation numerator(supervision, nnet_output);
    // note: supervision.weight is included as a factor in the derivative from
    // the numerator object, as well as the returned logprob.
    num_logprob_weighted = numerator.Forward();

    if (xent_output_deriv) {
      numerator.Backward(xent_output_deriv);
      if (nnet_output_deriv)
        nnet_output_deriv->AddMat(1.0, *xent_output_deriv);
    } else if (nnet_output_deriv) {
      numerator.Backward(nnet_output_deriv);
    }
  }

  *objf = num_logprob_weighted - den_logprob_weighted;
  *weight = supervision.weight * supervision.num_sequences *
      supervision.frames_per_sequence;
  if (!((*objf) - (*objf) == 0) || !ok) {
    // inf or NaN detected, or denominator computation returned false.
    if (nnet_output_deriv)
      nnet_output_deriv->SetZero();
    if (xent_output_deriv)
      xent_output_deriv->SetZero();
    BaseFloat default_objf = -10;
    KALDI_WARN << "Objective function is " << (*objf)
               << " and denominator computation (if done) returned "
               << std::boolalpha << ok
               << ", setting objective function to " << default_objf
               << " per frame.";
    *objf  = default_objf * *weight;
  }

  // This code helps us see how big the derivatives are, on average,
  // for different frames of the sequences.  As expected, they are
  // smaller towards the edges of the sequences (due to the penalization
  // of 'incorrect' pdf-ids.
  if (GetVerboseLevel() >= 1 && nnet_output_deriv != NULL && RandInt(0, 10) == 0) {
    int32 tot_frames = nnet_output_deriv->NumRows(),
 frames_per_sequence = supervision.frames_per_sequence,
       num_sequences = supervision.num_sequences;
    CuVector<BaseFloat> row_products(tot_frames);
    row_products.AddDiagMat2(1.0, *nnet_output_deriv, kNoTrans, 0.0);
    Vector<BaseFloat> row_products_cpu(row_products);
    Vector<BaseFloat> row_products_per_frame(frames_per_sequence);
    for (int32 i = 0; i < tot_frames; i++)
      row_products_per_frame(i / num_sequences) += row_products_cpu(i);
    KALDI_LOG << "Derivs per frame are " << row_products_per_frame;
  }

  if (opts.l2_regularize == 0.0) {
    *l2_term = 0.0;
  } else {
    // compute the l2 penalty term and its derivative
    BaseFloat scale = supervision.weight * opts.l2_regularize;
    *l2_term = -0.5 * scale * TraceMatMat(nnet_output, nnet_output, kTrans);
    if (nnet_output_deriv)
      nnet_output_deriv->AddMat(-1.0 * scale, nnet_output);
  }
}

void ComputeChainSmbrObjfAndDeriv(const ChainTrainingOptions &opts,
                                  const DenominatorGraph &den_graph,
                                  const Supervision &supervision,
                                  const CuMatrixBase<BaseFloat> &nnet_output,
                                  BaseFloat *objf,
                                  BaseFloat *mmi_objf,
                                  BaseFloat *l2_term,
                                  BaseFloat *weight,
                                  CuMatrixBase<BaseFloat> *nnet_output_deriv,
                                  CuMatrix<BaseFloat> *xent_output_deriv,
                                  const CuArray<int32> *sil_indices) {
  // num_posteriors is a matrix of size 
  // (num_sequences * frames_per_sequence) x num_pdfs and is ordered in the 
  // same way as nnet_output is i.e.
  // first the first frame of each sequence, then the second frame of 
  // each sequence, and so on.
  CuMatrix<BaseFloat> num_posteriors(nnet_output.NumRows(),
                                     nnet_output.NumCols());

  BaseFloat num_logprob_weighted;
  {
    NumeratorComputation numerator(supervision, nnet_output);
    // note: supervision.weight is included as a factor in the derivative from
    // the numerator object, and the logprob too.
    num_logprob_weighted = opts.mmi_factor * numerator.Forward();
    numerator.Backward(&num_posteriors);
#if HAVE_CUDA == 1
    if (!CuDevice::Instantiate().Enabled() && GetVerboseLevel() >= 2) {
      Posterior post(num_posteriors.NumRows());
      for (int32 i = 0; i < num_posteriors.NumRows(); i++) {
        CuSubVector<BaseFloat> row(num_posteriors, i);
        for (int32 j = 0; j < row.Dim(); j++) {
          BaseFloat p = row(j);
          if (p >= 0.01) {
            post[i].push_back(std::make_pair(j, p));
          }
        }
      }
      PosteriorHolder::Write(KALDI_LOG, false, post);
    }
#endif

    if (nnet_output_deriv && opts.mmi_factor != 0.0) {
      nnet_output_deriv->CopyFromMat(num_posteriors);
      nnet_output_deriv->Scale(opts.mmi_factor);
    }

    if (xent_output_deriv) {
      xent_output_deriv->CopyFromMat(num_posteriors);
    }
  }

  if (sil_indices && opts.exclude_silence) {
    // Exclude numerator posteriors for silence pdfs from accuracy 
    // computation. This is done by setting silence pdf posteiors to zero.
    // sil_indices is expected to have -1 at the indexes corresponding to 
    // silence pdfs, and "i" for any other index "i".
    num_posteriors.CopyCols(num_posteriors, *sil_indices);
  } else if (sil_indices && opts.one_silence_class) {
    // Create a copy with only the silence pdf posteriors.
    CuMatrix<BaseFloat> silence_post(nnet_output.NumRows(),
                                     nnet_output.NumCols());
    silence_post.CopyCols(num_posteriors, *sil_indices);

    // Sum the posteriors of silence pdfs to get posterior of silence class.
    CuVector<BaseFloat> total_silence_post(nnet_output.NumRows());
    total_silence_post.AddColSumMat(1.0, silence_post, 0.0);

    // Copy the silence class posterior to the columns of the silence pdfs.
    num_posteriors.CopyColsFromVec(total_silence_post, *sil_indices);
  }

  DenominatorSmbrComputation denominator(opts, den_graph,
                                         supervision.num_sequences,
                                         nnet_output, num_posteriors);

  BaseFloat den_logprob_negated;
  BaseFloat smbr_objf = denominator.ForwardSmbr(&den_logprob_negated);

  //if (opts.mmi_factor != 0.0) {
  //  DenominatorComputation denominator_mmi(opts, den_graph,
  //                                         supervision.num_sequences,
  //                                         nnet_output);
  //  KALDI_ASSERT(kaldi::ApproxEqual(-den_logprob_negated, opts.mmi_factor * denominator_mmi.Forward()));
  //}

  bool ok = true;
  if (nnet_output_deriv) {
    if (opts.mmi_factor == 0.0) nnet_output_deriv->SetZero();
    ok = denominator.BackwardSmbr(supervision.weight, nnet_output_deriv);
  }

  *objf = supervision.weight * smbr_objf;
  *mmi_objf = supervision.weight * den_logprob_negated + num_logprob_weighted;
  *weight = supervision.weight * supervision.num_sequences *
      supervision.frames_per_sequence;
  
  BaseFloat total_objf = *objf + *mmi_objf;
  if (!((total_objf) - (total_objf) == 0) || !ok) {
    // inf or NaN detected, or denominator computation returned false.
    if (nnet_output_deriv)
      nnet_output_deriv->SetZero();
    if (xent_output_deriv)
      xent_output_deriv->SetZero();
    BaseFloat default_objf = -opts.mmi_factor * 10;
    KALDI_WARN << "Objective function is " << (total_objf)
               << " and denominator computation (if done) returned "
               << std::boolalpha << ok
               << ", setting objective function to " << default_objf
               << " per frame.";
    *mmi_objf  = default_objf * *weight;
    *objf = 0.0;
  }

  // This code helps us see how big the derivatives are, on average,
  // for different frames of the sequences.  As expected, they are
  // smaller towards the edges of the sequences (due to the penalization
  // of 'incorrect' pdf-ids.
  if (GetVerboseLevel() >= 1 && nnet_output_deriv != NULL) {
    int32 tot_frames = nnet_output_deriv->NumRows(),
 frames_per_sequence = supervision.frames_per_sequence,
       num_sequences = supervision.num_sequences;
    CuVector<BaseFloat> row_products(tot_frames);
    row_products.AddDiagMat2(1.0, *nnet_output_deriv, kNoTrans, 0.0);
    Vector<BaseFloat> row_products_cpu(row_products);
    Vector<BaseFloat> row_products_per_frame(frames_per_sequence);
    for (int32 i = 0; i < tot_frames; i++)
      row_products_per_frame(i / num_sequences) += row_products_cpu(i);
    KALDI_LOG << "Derivs per frame are " << row_products_per_frame;
  }

  if (opts.l2_regularize == 0.0) {
    *l2_term = 0.0;
  } else if (!opts.norm_regularize) {
    // compute the l2 penalty term and its derivative
    BaseFloat scale = supervision.weight * opts.l2_regularize;
    *l2_term = -0.5 * scale * TraceMatMat(nnet_output, nnet_output, kTrans);
    if (nnet_output_deriv)
      nnet_output_deriv->AddMat(-1.0 * scale, nnet_output);
  } else {
    // compute the l2 penalty term and its derivative
    BaseFloat scale = supervision.weight * opts.l2_regularize;
    CuMatrix<BaseFloat> exp_nnet_output(nnet_output);
    exp_nnet_output.ApplyExp();
    *l2_term = -scale * exp_nnet_output.Sum();
    if (nnet_output_deriv)
      nnet_output_deriv->AddMat(-1.0 * scale, exp_nnet_output);
  }
}


}  // namespace chain
}  // namespace kaldi
