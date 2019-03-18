#ifndef TH_GENERIC_FILE
#define TH_GENERIC_FILE "generic/SpatialDepthWiseConvolution.c"
#else

static inline void THNN_(SpatialDepthWiseConvolution_shapeCheck)(
	THTensor *input, THTensor *gradOutput,
	THTensor *weight, THTensor *bias,
	int kH, int kW, int dH, int dW, int padH, int padW) {

  THArgCheck(kW > 0 && kH > 0, 9,
	       "kernel size should be greater than zero, but got kH: %d kW: %d", kH, kW);
  THArgCheck(dW > 0 && dH > 0, 11,
	     "stride should be greater than zero, but got dH: %d dW: %d", dH, dW);
  THNN_ARGCHECK(weight->nDimension == 4, 5, weight,
		"2D or 4D weight tensor expected, but got: %s");

  if (bias != NULL) {
    THNN_CHECK_DIM_SIZE(bias, 2, 0, weight->size[0]);
    THNN_CHECK_DIM_SIZE(bias, 2, 1, weight->size[1]);
  }

  int ndim = input->nDimension;
  int dimf = 0;
  int dimh = 1;
  int dimw = 2;

  if (ndim == 4) {
    dimf++;
    dimh++;
    dimw++;
  }

  THNN_ARGCHECK(ndim == 3 || ndim == 4, 2, input,
		"3D or 4D input tensor expected but got: %s");

  long nInputPlane  = weight->size[1];
  long inputHeight  = input->size[dimh];
  long inputWidth   = input->size[dimw];
  long nOutputPlane = weight->size[0];
  long outputHeight = (inputHeight + 2*padH - kH) / dH + 1;
  long outputWidth  = (inputWidth + 2*padW - kW) / dW + 1;

  if (outputWidth < 1 || outputHeight < 1)
    THError("Given input size: (%d x %d x %d). "
	    "Calculated output size: (%d x %d x %d). Output size is too small",
	    nInputPlane,inputHeight,inputWidth,nOutputPlane*nInputPlane,outputHeight,outputWidth);

  THNN_CHECK_DIM_SIZE(input, ndim, dimf, nInputPlane);

  if (gradOutput != NULL) {
    THNN_CHECK_DIM_SIZE(gradOutput, ndim + 1, dimf, nInputPlane);
    THNN_CHECK_DIM_SIZE(gradOutput, ndim + 1, dimh, nOutputPlane);
    THNN_CHECK_DIM_SIZE(gradOutput, ndim + 1, dimw, outputHeight);
    THNN_CHECK_DIM_SIZE(gradOutput, ndim + 1, dimw + 1, outputWidth);
  }
}

static void THNN_(SpatialDepthWiseConvolution_updateOutput_frame)(
          THTensor *input,
          THTensor *output,
          THTensor *weight,
          THTensor *bias,
          THTensor *finput,
          int kW,
          int kH,
          int dW,
          int dH,
          int padW,
          int padH,
          long nInputPlane,
          long inputWidth,
          long inputHeight,
          long nOutputPlane,
          long outputWidth,
          long outputHeight)
{
  long i;
  THTensor *output2d;

  THNN_(unfolded_copy)(finput, input, kW, kH, dW, dH, padW, padH,
		       nInputPlane, inputWidth, inputHeight,
		       outputWidth, outputHeight);

  output2d = THTensor_(newWithStorage2d)(output->storage, output->storageOffset,
                                         nOutputPlane, -1,
                                         outputHeight*outputWidth, -1);
  if (bias) {
    for(i = 0; i < nOutputPlane; i++)
        THVector_(fill)
	  (output->storage->data + output->storageOffset + output->stride[0] * i,
	   THTensor_(get1d)(bias, i), outputHeight*outputWidth);
  } else {
    THTensor_(zero)(output);
  }

  THTensor_(addmm)(output2d, 1, output2d, 1, weight, finput);

  THTensor_(free)(output2d);
}

void THNN_(SpatialDepthWiseConvolution_updateOutput)(
          THNNState *state,
          THTensor *input,
          THTensor *output,
          THTensor *weight,
          THTensor *bias,
          THTensor *finput,
          THTensor *fgradInput,
          int kW,
          int kH,
          int dW,
          int dH,
          int padW,
          int padH)
{
  long nInputPlane = weight->nDimension == 2 ? weight->size[1]/(kH*kW) : weight->size[1];
  long nOutputPlane = weight->size[0];
  if (weight->nDimension == 2) {
    THTensor_(resize4d)(weight, nOutputPlane, nInputPlane, kH, kW);
  }

  THNN_(SpatialDepthWiseConvolution_shapeCheck)
    (input, NULL, weight, bias, kH, kW, dH, dW, padH, padW);

  THTensor *weight_t = THTensor_(newTranspose)(weight, 0, 1);
  THTensor *weight_tc = THTensor_(newContiguous)(weight_t);

  THTensor *bias_t = NULL;
  THTensor *bias_tc = NULL;
  if(bias) {
    bias_t = THTensor_(newTranspose)(bias, 0, 1);
    bias_tc = THTensor_(newContiguous)(bias_t);
  }

  // resize weight
  long s1 = weight_tc->size[0];
  long s2 = weight_tc->size[1];
  long s3 = weight_tc->size[2] * weight_tc->size[3];
  THTensor *weight_rs = THTensor_(newWithStorage3d)(weight_tc->storage, weight_tc->storageOffset,
          s1, -1, s2, -1, s3, -1);

  THTensor *input_c = THTensor_(newContiguous)(input);

  int ndim = input_c->nDimension;

  int batch = 1;
  if (ndim == 3) {
    // Force batch
    batch = 0;
    THTensor_(resize4d)(input_c, 1, input_c->size[0], input_c->size[1], input_c->size[2]);
  }

  long inputHeight  = input_c->size[2];
  long inputWidth   = input_c->size[3];
  long outputHeight = (inputHeight + 2*padH - kH) / dH + 1;
  long outputWidth  = (inputWidth + 2*padW - kW) / dW + 1;

  long T = input_c->size[0];
  long t;

  THTensor_(resize5d)(output, T, nInputPlane, nOutputPlane, outputHeight, outputWidth);
  THTensor_(resize4d)(finput, T, nInputPlane, kW*kH*1, outputHeight*outputWidth);

#pragma omp parallel for private(t)
  for(t = 0; t < T; t++)
  {
    THTensor *input_t = THTensor_(newSelect)(input_c, 0, t);
    THTensor *output_t = THTensor_(newSelect)(output, 0, t);
    THTensor *finput_t = THTensor_(newSelect)(finput, 0, t);

    long i;
#pragma omp parallel for private(i)
    for(i = 0; i < nInputPlane; i++)
    {
      THTensor *weight_i = THTensor_(newSelect)(weight_rs, 0, i);
      THTensor *input_i = THTensor_(newNarrow)(input_t, 0, i, 1);
      THTensor *output_i = THTensor_(newSelect)(output_t, 0, i);
      THTensor *finput_i = THTensor_(newSelect)(finput_t, 0, i);
      THTensor *bias_i = NULL;
      if(bias_tc) {
        bias_i = THTensor_(newSelect)(bias_tc, 0, i);
      }
      THNN_(SpatialDepthWiseConvolution_updateOutput_frame)
	(input_i, output_i, weight_i, bias_i, finput_i,
	 kW, kH, dW, dH, padW, padH,
	 1, inputWidth, inputHeight,
	 nOutputPlane, outputWidth, outputHeight);

      THTensor_(free)(input_i);
      THTensor_(free)(weight_i);
      THTensor_(free)(bias_i);
      THTensor_(free)(output_i);
      THTensor_(free)(finput_i);
    }
    THTensor_(free)(input_t);
    THTensor_(free)(output_t);
    THTensor_(free)(finput_t);
  }

  THTensor_(free)(weight_t);
  THTensor_(free)(weight_tc);
  THTensor_(free)(weight_rs);
  THTensor_(free)(bias_t);
  THTensor_(free)(bias_tc);
  THTensor_(resize4d)(output, T, nInputPlane * nOutputPlane, outputHeight, outputWidth);

  if (batch == 0) {
    THTensor_(select)(output, NULL, 0, 0);
    THTensor_(select)(input_c, NULL, 0, 0);
    THTensor_(select)(finput, NULL, 0, 0);
  }
  THTensor_(free)(input_c);
}

static void THNN_(SpatialDepthWiseConvolution_updateGradInput_frame)(
          THTensor *gradInput,
          THTensor *gradOutput,
          THTensor *weight,
          THTensor *fgradInput,
          int kW,
          int kH,
          int dW,
          int dH,
          int padW,
          int padH)
{
  THTensor *gradOutput2d = THTensor_(newWithStorage2d)
    (gradOutput->storage, gradOutput->storageOffset,
     gradOutput->size[0], -1,
     gradOutput->size[1]*gradOutput->size[2], -1);
  THTensor_(addmm)(fgradInput, 0, fgradInput, 1, weight, gradOutput2d);
  THTensor_(free)(gradOutput2d);

  THTensor_(zero)(gradInput);

  THNN_(unfolded_acc)(fgradInput, gradInput, kW, kH, dW, dH,
		      padW, padH,
		      gradInput->size[0], gradInput->size[2], gradInput->size[1],
		      gradOutput->size[2], gradOutput->size[1]);
}

void THNN_(SpatialDepthWiseConvolution_updateGradInput)(
          THNNState *state,
          THTensor *input,
          THTensor *gradOutput,
          THTensor *gradInput,
          THTensor *weight,
          THTensor *finput,
          THTensor *fgradInput,
          int kW,
          int kH,
          int dW,
          int dH,
          int padW,
          int padH)
{
  long nInputPlane = weight->nDimension == 2 ? weight->size[1]/(kH*kW) : weight->size[1];
  long nOutputPlane = weight->size[0];
  if (weight->nDimension == 2) {
    THTensor_(resize4d)(weight, nOutputPlane, nInputPlane, kH, kW);
  }
  THTensor *gradOutput_work = THTensor_(newWithTensor)(gradOutput);

  if (input->nDimension == 3) {
    if (gradOutput_work->nDimension == 3) {
      THTensor_(resize4d)(gradOutput_work, nInputPlane, nOutputPlane, gradOutput_work->size[1], gradOutput_work->size[2]);
    }
  }
  else
  {
    if (gradOutput_work->nDimension == 4) {
      THTensor_(resize5d)(gradOutput_work, gradOutput_work->size[0], nInputPlane, nOutputPlane, gradOutput_work->size[2], gradOutput_work->size[3]);
    }
  }


  THNN_(SpatialDepthWiseConvolution_shapeCheck)
    (input, gradOutput_work, weight, NULL, kH, kW, dH, dW, padH, padW);

  THTensor *weight_t = THTensor_(newTranspose)(weight, 0, 1);
  THTensor *weight_tc = THTensor_(newContiguous)(weight_t);


  // resize weight
  long s1 = weight_tc->size[0];
  long s2 = weight_tc->size[1];
  long s3 = weight_tc->size[2] * weight_tc->size[3];
  THTensor *weight_rs = THTensor_(newWithStorage3d)(weight_tc->storage, weight_tc->storageOffset,
          s1, -1, s2, -1, s3, -1);

  THTensor *input_c = THTensor_(newContiguous)(input);

  int batch = 1;
  if (input_c->nDimension == 3) {
    // Force batch
    batch = 0;
    THTensor_(resize4d)(input_c, 1, input_c->size[0], input_c->size[1], input_c->size[2]);
    THTensor_(resize5d)(gradOutput_work, 1, gradOutput_work->size[0], gradOutput_work->size[1], gradOutput_work->size[2], gradOutput_work->size[3]);
  }

  long inputHeight  = input_c->size[2];
  long inputWidth   = input_c->size[3];
  long outputHeight = (inputHeight + 2*padH - kH) / dH + 1;
  long outputWidth  = (inputWidth + 2*padW - kW) / dW + 1;

  long T = input_c->size[0];
  long t;

  THTensor_(resizeAs)(gradInput, input_c);
  THTensor_(resize4d)(fgradInput, T, nInputPlane, kW*kH*1, outputHeight*outputWidth);

  // depending on the BLAS library, fgradInput (result tensor) might
  // be left uninitialized on zero alpha, which might lead to weird behavior
  // hence, to be safe, zero it
  THTensor_(zero)(fgradInput);



#pragma omp parallel for private(t)
  for(t = 0; t < T; t++)
  {
    THTensor *gradInput_t = THTensor_(newSelect)(gradInput, 0, t);
    THTensor *gradOutput_t = THTensor_(newSelect)(gradOutput_work, 0, t);
    THTensor *fgradInput_t = THTensor_(newSelect)(fgradInput, 0, t);


    long i;
#pragma omp parallel for private(i)
    for(i = 0; i < nInputPlane; i++)
    {
      THTensor *weight_i = THTensor_(newSelect)(weight_rs, 0, i);
      THTensor *gradInput_i = THTensor_(newNarrow)(gradInput_t, 0, i, 1);
      THTensor *gradOutput_i = THTensor_(newSelect)(gradOutput_t, 0, i);
      THTensor *fgradInput_i = THTensor_(newSelect)(fgradInput_t, 0, i);

      THTensor_(transpose)(weight_i, weight_i, 0, 1);

      THNN_(SpatialDepthWiseConvolution_updateGradInput_frame)(gradInput_i, gradOutput_i,
              weight_i, fgradInput_i,
              kW, kH, dW, dH, padW, padH);

      THTensor_(free)(gradInput_i);
      THTensor_(free)(weight_i);
      THTensor_(free)(gradOutput_i);
      THTensor_(free)(fgradInput_i);
    }

    THTensor_(free)(gradInput_t);
    THTensor_(free)(gradOutput_t);
    THTensor_(free)(fgradInput_t);
  }

  if (batch == 0) {
    THTensor_(select)(gradOutput_work, NULL, 0, 0);
    THTensor_(select)(input_c, NULL, 0, 0);
    THTensor_(select)(gradInput, NULL, 0, 0);
    THTensor_(select)(fgradInput, NULL, 0, 0);
  }

  THTensor_(free)(input_c);
  THTensor_(free)(gradOutput_work);
  THTensor_(free)(weight_t);
  THTensor_(free)(weight_tc);
  THTensor_(free)(weight_rs);
}

static void THNN_(SpatialDepthWiseConvolution_accGradParameters_frame)(
          THTensor *gradOutput,
          THTensor *gradWeight,
          THTensor *gradBias,
          THTensor *finput,
          accreal scale)
{
  long i;
  THTensor *gradOutput2d = THTensor_(newWithStorage2d)
    (gradOutput->storage, gradOutput->storageOffset,
     gradOutput->size[0], -1,
     gradOutput->size[1]*gradOutput->size[2], -1);

  THTensor_(transpose)(finput, finput, 0, 1);
  THTensor_(addmm)(gradWeight, 1, gradWeight, scale, gradOutput2d, finput);
  THTensor_(transpose)(finput, finput, 0, 1);

  if (gradBias) {
    for(i = 0; i < gradBias->size[0]; i++)
    {
      long k;
      real sum = 0;
      real *data = gradOutput2d->storage->data + gradOutput2d->storageOffset + i*gradOutput2d->stride[0];
      for(k = 0; k < gradOutput2d->size[1]; k++)
        sum += data[k];
      (gradBias->storage->data + gradBias->storageOffset)[i] += scale*sum;
    }
  }

  THTensor_(free)(gradOutput2d);
}

void THNN_(SpatialDepthWiseConvolution_accGradParameters)(
          THNNState *state,
          THTensor *input,
          THTensor *gradOutput,
          THTensor *gradWeight,
          THTensor *gradBias,
          THTensor *finput,
          THTensor *fgradInput,
          int kW,
          int kH,
          int dW,
          int dH,
          int padW,
          int padH,
          accreal scale)
{
  long nInputPlane = gradWeight->nDimension == 2 ? gradWeight->size[1]/(kH*kW) : gradWeight->size[1];
  long nOutputPlane = gradWeight->size[0];
  if (gradWeight->nDimension == 2) {
    THTensor_(resize4d)(gradWeight, nOutputPlane, nInputPlane, kH, kW);
  }

  THTensor *gradOutput_work = THTensor_(newWithTensor)(gradOutput);
  if (input->nDimension == 3) {
    if (gradOutput_work->nDimension == 3) {
      THTensor_(resize4d)(gradOutput_work, nInputPlane, nOutputPlane, gradOutput_work->size[1], gradOutput_work->size[2]);
    }
  }
  else
  {
    if (gradOutput_work->nDimension == 4) {
      THTensor_(resize5d)(gradOutput_work, gradOutput_work->size[0], nInputPlane, nOutputPlane, gradOutput_work->size[2], gradOutput_work->size[3]);
    }
  }


  THNN_(SpatialDepthWiseConvolution_shapeCheck)
    (input, gradOutput_work, gradWeight, gradBias, kH, kW, dH, dW, padH, padW);

  // Transpose gradWeight & gradBias
  THTensor_(transpose)(gradWeight, NULL, 0, 1);
  THTensor *gradWeight_c = THTensor_(newContiguous)(gradWeight);

  THTensor *gradBias_c = NULL;
  if(gradBias) {
	  THTensor_(transpose)(gradBias, NULL, 0, 1);
	  gradBias_c = THTensor_(newContiguous)(gradBias);
  }

  // resize gradWeight
  long s1 = gradWeight_c->size[0];
  long s2 = gradWeight_c->size[1];
  long s3 = gradWeight_c->size[2] * gradWeight_c->size[3];
  THTensor *gradWeight_rs = THTensor_(newWithStorage3d)(gradWeight_c->storage, gradWeight_c->storageOffset,
          s1, -1, s2, -1, s3, -1);

  THTensor *input_c = THTensor_(newContiguous)(input);


  int batch = 1;
  if (input_c->nDimension == 3) {
    // Force batch
    batch = 0;
    THTensor_(resize4d)(input_c, 1, input_c->size[0], input_c->size[1], input_c->size[2]);
    THTensor_(resize5d)(gradOutput_work, 1, gradOutput_work->size[0], gradOutput_work->size[1], gradOutput_work->size[2], gradOutput_work->size[3]);
  }

  long inputHeight  = input_c->size[2];
  long inputWidth   = input_c->size[3];
  long outputHeight = (inputHeight + 2*padH - kH) / dH + 1;
  long outputWidth  = (inputWidth + 2*padW - kW) / dW + 1;

  long T = input_c->size[0];
  long t;
  THTensor_(resize4d)(finput, T, nInputPlane, kW*kH*1, outputHeight*outputWidth);

  for(t = 0; t < T; t++)
  {
    THTensor *gradOutput_t = THTensor_(newSelect)(gradOutput_work, 0, t);
    THTensor *finput_t = THTensor_(newSelect)(finput, 0, t);
    long i;
#pragma omp parallel for private(i)
    for(i = 0; i < nInputPlane; i++)
    {
      THTensor *finput_i = THTensor_(newSelect)(finput_t, 0, i);
      THTensor *gradOutput_i = THTensor_(newSelect)(gradOutput_t, 0, i);
      THTensor *gradWeight_i = THTensor_(newSelect)(gradWeight_rs, 0, i);
      THTensor *gradBias_i = NULL;
      if(gradBias) {
      	gradBias_i = THTensor_(newSelect)(gradBias_c, 0, i);
      }
      THNN_(SpatialDepthWiseConvolution_accGradParameters_frame)(gradOutput_i, gradWeight_i,
                gradBias_i, finput_i, scale);

      THTensor_(free)(finput_i);
      THTensor_(free)(gradOutput_i);
      THTensor_(free)(gradWeight_i);
      THTensor_(free)(gradBias_i);
    }

    THTensor_(free)(gradOutput_t);
    THTensor_(free)(finput_t);
  }

  // Copy back and transpose back
  THTensor_(transpose)(gradWeight, NULL, 0, 1);
  THTensor_(resize4d)(gradWeight, nInputPlane, nOutputPlane, kH, kW);
  THTensor_(copy)(gradWeight, gradWeight_rs);
  THTensor_(transpose)(gradWeight, NULL, 0, 1);

  if(gradBias) {
	  THTensor_(transpose)(gradBias, NULL, 0, 1);
	  THTensor_(resize2d)(gradBias, nInputPlane, nOutputPlane);
	  THTensor_(copy)(gradBias, gradBias_c);
	  THTensor_(transpose)(gradBias, NULL, 0, 1);
  }

  if (batch == 0) {
    THTensor_(select)(gradOutput_work, NULL, 0, 0);
    THTensor_(select)(input_c, NULL, 0, 0);
    THTensor_(select)(finput, NULL, 0, 0);
  }

  THTensor_(free)(input_c);
  THTensor_(free)(gradOutput_work);
  THTensor_(free)(gradWeight_c);
  THTensor_(free)(gradWeight_rs);
  THTensor_(free)(gradBias_c);
}

#endif
