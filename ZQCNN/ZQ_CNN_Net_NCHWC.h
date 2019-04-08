#ifndef _ZQ_CNN_NET_NCHWC_H_
#define _ZQ_CNN_NET_NCHWC_H_
#pragma once
#include "ZQ_CNN_Layer_NCHWC.h"
#include <map>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
namespace ZQ
{
	template<class Tensor4D>
	class ZQ_CNN_Net_NCHWC
	{
		using My_CNN_Layer = ZQ_CNN_Layer_NCHWC<Tensor4D>;
	protected:
		class Buffer
		{
		public:
			void* data;
			__int64 len;

			Buffer() : data(0), len(0) {}
			~Buffer() { Release(); }
			void Release() { if (data) _aligned_free(data); data = 0; len = 0; }
		};

	public:
		ZQ_CNN_Net_NCHWC() :has_input_layer(false), show_debug_info(false), use_buffer(true),
			has_innerproduct_layer(false), ignore_small_value(0) {}
		~ZQ_CNN_Net_NCHWC() { _clear(); };

	private:
		std::vector<My_CNN_Layer*> layers;
		std::vector<std::string> layer_type_names;
		std::vector<Tensor4D*> blobs; //blobs[0] stores a pointer to input blob
		std::map<std::string, int> map_name_to_layer_idx;
		std::map<std::string, int> map_name_to_blob_idx;
		std::map<int, int> simplify_inplace_blob_map;
		std::vector<std::vector<int> > bottoms;
		std::vector<std::vector<int> > tops;	//tops[0][0] stores input blob pointer
		std::string input_name;
		bool has_input_layer;
		bool show_debug_info;
		bool use_buffer;
		float ignore_small_value;
		Buffer _buffer;
		bool has_innerproduct_layer;
		int input_C, input_H, input_W;
	public:
		void TurnOnShowDebugInfo() { show_debug_info = true; }
		void TurnOffShowDebugInfo() { show_debug_info = false; }
		void TurnOnUseBuffer() { use_buffer = true; }
		void TurnOffUseBuffer() { use_buffer = false; }
		void GetInputDim(int& in_C, int& in_H, int& in_W)const { in_C = input_C; in_H = input_H; in_W = input_W; }
		bool LoadFrom(const std::string& param_file, const std::string& model_file, bool merge_bn = false, float ignore_small_value = 1e-12,
			bool merge_prelu = false)
		{
			_clear();
			this->ignore_small_value = ignore_small_value;
			if (!_load_param_file(param_file))
			{
				_clear();
				return false;
			}
			if (!_check_connect())
			{
				_clear();
				return false;
			}
			if (!_load_model_file(model_file))
			{
				_clear();
				return false;
			}

			_simplify_inplace();

			if (merge_bn)
			{
				if (!_merge_bn())
					return false;
			}
			if (merge_prelu)
			{
				if (!_merge_prelu())
					return false;
			}
			_prepack();
			return true;
		}

		bool SaveModel(const std::string& file) const
		{
			return _save_model_file(file);
		}

		
		bool LoadFromBuffer(const char*& param_buffer, __int64 param_buffer_len, const char*& model_buffer, __int64 model_buffer_len,
			bool merge_bn = false, float ignore_small_value = 1e-12, bool merge_prelu = false)
		{
			_clear();
			this->ignore_small_value = ignore_small_value;
			if (!_load_param_from_buffer(param_buffer, param_buffer_len))
			{
				_clear();
				return false;
			}
			if (!_check_connect())
			{
				_clear();
				return false;
			}
			if (!_load_model_from_buffer(model_buffer, model_buffer_len))
			{
				_clear();
				return false;
			}

			_simplify_inplace();

			if (merge_bn)
			{
				if (!_merge_bn())
					return false;
			}
			if (merge_prelu)
			{
				if (!_merge_prelu())
					return false;
			}
			_prepack();
			return true;
		}

		__int64 GetNumOfMulAdd() const
		{
			__int64 sum = 0;
			for (int i = 0; i < layers.size(); i++)
				sum += layers[i]->GetNumOfMulAdd();
			return sum;
		}

		__int64 GetNumOfMulAddConv() const
		{
			__int64 sum = 0;
			for (int i = 0; i < layers.size(); i++)
			{
				if (My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), "Convolution") == 0
					|| My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), "InnerProduct") == 0)
					sum += layers[i]->GetNumOfMulAdd();
			}
			return sum;
		}
		__int64 GetNumOfMulAddDwConv() const
		{
			__int64 sum = 0;
			for (int i = 0; i < layers.size(); i++)
			{
				if (My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), "DepthwiseConvolution") == 0)
					sum += layers[i]->GetNumOfMulAdd();
			}
			return sum;
		}

		float GetLastTimeOfLayerType(const std::string& layer_typename) const
		{
			float sum = 0;
			for (int i = 0; i < layers.size(); i++)
			{
				if (My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), layer_typename.c_str()) == 0)
					sum += layers[i]->last_cost_time;
			}
			return sum;
		}


		/*it may change input in case of padding, but the data will not be lost*/
		bool Forward(Tensor4D& input)
		{
			if (map_name_to_blob_idx.size() == 0 || map_name_to_layer_idx.size() == 0 || tops.size() == 0)
				return false;
			if (has_innerproduct_layer)
			{
				if (input.GetH() != input_H || input.GetW() != input_W || input.GetC() != input_C)
				{
					std::cout << "The dimenson doesnot match with the needed\n";
					return false;
				}
			}
			blobs[0] = &input;

			for (int i = 0; i < layers.size(); i++)
			{
				std::vector<Tensor4D*> bottom_ptrs, top_ptrs;
				for (int j = 0; j < bottoms[i].size(); j++)
					bottom_ptrs.push_back(blobs[bottoms[i][j]]);
				for (int j = 0; j < tops[i].size(); j++)
					top_ptrs.push_back(blobs[tops[i][j]]);

				layers[i]->show_debug_info = show_debug_info;
				//printf("%d\n", i);
				layers[i]->use_buffer = use_buffer;
				layers[i]->buffer = &(_buffer.data);
				layers[i]->buffer_len = &(_buffer.len);
				if (!layers[i]->Forward(&bottom_ptrs, &top_ptrs))
				{
					blobs[0] = 0;
					tops[0][0] = 0;
					printf("failed to run layer: %s\n", layers[i]->name.c_str());
					return false;
				}
//				char buf[100];
//#if defined(_WIN32)
//				sprintf_s(buf, "NCHWC_%d.txt", i);
//#else
//				sprintf(buf, "NCHWC_%d.txt", i);
//#endif
//				top_ptrs[0]->SaveToFile(buf);
			}

			blobs[0] = 0;
			tops[0][0] = 0;
			return true;
		}

		bool Forward(Tensor4D& input, const std::string& start_layer_name, const std::string& end_layer_name)
		{
			if (map_name_to_blob_idx.size() == 0 || map_name_to_layer_idx.size() == 0 || tops.size() == 0)
				return false;
			if (has_innerproduct_layer)
			{
				if (input.GetH() != input_H || input.GetW() != input_W || input.GetC() != input_C)
				{
					std::cout << "The dimenson doesnot match with the needed\n";
					return false;
				}
			}
			blobs[0] = &input;

			bool has_begin = false, has_end = false;
			for (int i = 0; i < layers.size(); i++)
			{
				if (My_CNN_Layer::_my_strcmpi(layers[i]->name.c_str(), start_layer_name.c_str()) == 0)
					has_begin = true;
				if (!has_begin)
					continue;
				std::vector<Tensor4D*> bottom_ptrs, top_ptrs;
				for (int j = 0; j < bottoms[i].size(); j++)
					bottom_ptrs.push_back(blobs[bottoms[i][j]]);
				for (int j = 0; j < tops[i].size(); j++)
					top_ptrs.push_back(blobs[tops[i][j]]);

				layers[i]->show_debug_info = show_debug_info;
				//printf("%d\n", i);
				layers[i]->use_buffer = use_buffer;
				layers[i]->buffer = &(_buffer.data);
				layers[i]->buffer_len = &(_buffer.len);
				if (!layers[i]->Forward(&bottom_ptrs, &top_ptrs))
				{
					blobs[0] = 0;
					tops[0][0] = 0;
					printf("failed to run layer: %s\n", layers[i]->name.c_str());
					return false;
				}
				if (My_CNN_Layer::_my_strcmpi(layers[i]->name.c_str(), end_layer_name.c_str()) == 0)
					has_end = true;
				if (has_end)
					break;
			}

			blobs[0] = 0;
			tops[0][0] = 0;
			return true;
		}

		const Tensor4D* GetBlobByName(std::string name)
		{
			std::map<std::string, int>::iterator it = map_name_to_blob_idx.find(name);
			if (it == map_name_to_blob_idx.end())
				return 0;
			else
			{
				if (simplify_inplace_blob_map.find(it->second) == simplify_inplace_blob_map.end())
					return blobs[it->second];
				else
					return blobs[simplify_inplace_blob_map[it->second]];
			}
		}

	private:
		void _clear()
		{
			for (int i = 0; i < layers.size(); i++)
			{
				if (layers[i])
					delete layers[i];
			}
			layers.clear();
			layer_type_names.clear();
			map_name_to_layer_idx.clear();
			//blob[0] is a pointer to input blob, DONT FREE
			for (int i = 1; i < blobs.size(); i++)
			{
				if (blobs[i])
					delete blobs[i];
			}
			blobs.clear();
			map_name_to_layer_idx.clear();
			has_input_layer = false;
		}

		bool _getline(std::fstream& fin, const char*& buffer, __int64& buffer_len, std::string& line)
		{
			if (buffer == 0)
			{
				if (!fin.is_open() || fin.eof())
					return false;
				std::getline(fin, line);
				return true;
			}
			else
			{
				if (buffer_len <= 0)
					return false;
				__int64 i = 0;
				for (; i < buffer_len; i++)
				{
					if (buffer[i] != '\n' && buffer[i] != '\r')
						break;
				}
				if (i == buffer_len)
					return false;

				__int64 j = i;
				for (; j < buffer_len; j++)
				{
					if (buffer[j] == '\n' || buffer[j] == '\r')
						break;
				}

				if (j == i)
					return false;
				__int64 cur_len = j - i;
				line.clear();
				line.append(buffer + i, cur_len);
				buffer += cur_len;
				buffer_len -= cur_len;
				return true;
			}
		}

		bool _load_param_file(const std::string& file)
		{
			std::fstream fin(file, std::ios::in);
			if (!fin.is_open())
			{
				std::cout << "failed to open file " << file << "\n";
				return false;
			}
			return _load_param_from_file_or_buffer(fin, NULL, 0);
		}

		bool _load_param_from_buffer(const char* buffer, __int64 buffer_len)
		{
			std::fstream fin;
			return _load_param_from_file_or_buffer(fin, buffer, buffer_len);
		}

		bool _load_param_from_file_or_buffer(std::fstream& fin, const char* buffer, __int64 buffer_len)
		{
			std::string line;
			int buf_len = 2000;
			std::vector<char> buf(buf_len + 1);
			while (_getline(fin, buffer, buffer_len, line))
			{
				buf[0] = '\0';
#if defined(_WIN32)
				if (sscanf_s(line.c_str(), "%s", &buf[0], buf_len) == 0)
					continue;
#else
				if (sscanf(line.c_str(), "%s", &buf[0]) == 0)
					continue;
#endif
				if (My_CNN_Layer::_my_strcmpi(&buf[0], "Convolution") == 0)
				{
					if (layers.size() == 0)
					{
						std::cout << "Input layer must be the first!\n";
						return false;
					}
					My_CNN_Layer* cur_layer = new ZQ_CNN_Layer_NCHWC_Convolution<Tensor4D>();
					if (cur_layer == 0) {
						std::cout << "failed to create a Convolution layer!\n";
						return false;
					}
					if (!_add_layer_and_blobs(cur_layer, line, false))
					{
						delete cur_layer;
						return false;
					}
					layer_type_names.push_back("Convolution");
				}
				else if (My_CNN_Layer::_my_strcmpi(&buf[0], "DepthwiseConvolution") == 0)
				{
					if (layers.size() == 0)
					{
						std::cout << "Input layer must be the first!\n";
						return false;
					}
					My_CNN_Layer* cur_layer = new ZQ_CNN_Layer_NCHWC_DepthwiseConvolution<Tensor4D>();
					if (cur_layer == 0) {
						std::cout << "failed to create a DepthwiseConvolution layer!\n";
						return false;
					}
					if (!_add_layer_and_blobs(cur_layer, line, false))
					{
						delete cur_layer;
						return false;
					}
					layer_type_names.push_back("DepthwiseConvolution");
				}
				else if (My_CNN_Layer::_my_strcmpi(&buf[0], "BatchNormScale") == 0)
				{
					if (layers.size() == 0)
					{
						std::cout << "Input layer must be the first!\n";
						return false;
					}
					My_CNN_Layer* cur_layer = new ZQ_CNN_Layer_NCHWC_BatchNormScale<Tensor4D>();
					if (cur_layer == 0) {
						std::cout << "failed to create a BatchNorm layer!\n";
						return false;
					}
					if (!_add_layer_and_blobs(cur_layer, line, false))
					{
						delete cur_layer;
						return false;
					}
					layer_type_names.push_back("BatchNormScale");
				}
				else if (My_CNN_Layer::_my_strcmpi(&buf[0], "PReLU") == 0)
				{
					if (layers.size() == 0)
					{
						std::cout << "Input layer must be the first!\n";
						return false;
					}
					My_CNN_Layer* cur_layer = new ZQ_CNN_Layer_NCHWC_PReLU<Tensor4D>();
					if (cur_layer == 0) {
						std::cout << "failed to create a PReLU layer!\n";
						return false;
					}
					if (!_add_layer_and_blobs(cur_layer, line, false))
					{
						delete cur_layer;
						return false;
					}
					layer_type_names.push_back("PReLU");
				}
				else if (My_CNN_Layer::_my_strcmpi(&buf[0], "ReLU") == 0)
				{
					if (layers.size() == 0)
					{
						std::cout << "Input layer must be the first!\n";
						return false;
					}
					My_CNN_Layer* cur_layer = new ZQ_CNN_Layer_NCHWC_ReLU<Tensor4D>();
					if (cur_layer == 0) {
						std::cout << "failed to create a ReLU layer!\n";
						return false;
					}
					if (!_add_layer_and_blobs(cur_layer, line, false))
					{
						delete cur_layer;
						return false;
					}
					layer_type_names.push_back("ReLU");
				}
				else if (My_CNN_Layer::_my_strcmpi(&buf[0], "Softmax") == 0)
				{
					if (layers.size() == 0)
					{
						std::cout << "Input layer must be the first!\n";
						return false;
					}
					My_CNN_Layer* cur_layer = new ZQ_CNN_Layer_NCHWC_Softmax<Tensor4D>();
					if (cur_layer == 0) {
						std::cout << "failed to create a Softmax layer!\n";
						return false;
					}
					if (!_add_layer_and_blobs(cur_layer, line, false))
					{
						delete cur_layer;
						return false;
					}
					layer_type_names.push_back("Softmax");
				}
				else if (My_CNN_Layer::_my_strcmpi(&buf[0], "Pooling") == 0)
				{
					if (layers.size() == 0)
					{
						std::cout << "Input layer must be the first!\n";
						return false;
					}
					My_CNN_Layer* cur_layer = new ZQ_CNN_Layer_NCHWC_Pooling<Tensor4D>();
					if (cur_layer == 0) {
						std::cout << "failed to create a Pooling layer!\n";
						return false;
					}
					if (!_add_layer_and_blobs(cur_layer, line, false))
					{
						delete cur_layer;
						return false;
					}
					layer_type_names.push_back("Pooling");
				}
				else if (My_CNN_Layer::_my_strcmpi(&buf[0], "InnerProduct") == 0)
				{
					has_innerproduct_layer = true;
					if (layers.size() == 0)
					{
						std::cout << "Input layer must be the first!\n";
						return false;
					}

					My_CNN_Layer* cur_layer = new ZQ_CNN_Layer_NCHWC_InnerProduct<Tensor4D>();
					if (cur_layer == 0) {
						std::cout << "failed to create a InnerProduct layer!\n";
						return false;
					}
					if (!_add_layer_and_blobs(cur_layer, line, false))
					{
						delete cur_layer;
						return false;
					}
					layer_type_names.push_back("InnerProduct");
				}
				else if (My_CNN_Layer::_my_strcmpi(&buf[0], "Eltwise") == 0)
				{
					if (layers.size() == 0)
					{
						std::cout << "Input layer must be the first!\n";
						return false;
					}
					My_CNN_Layer* cur_layer = new ZQ_CNN_Layer_NCHWC_Eltwise<Tensor4D>();
					if (cur_layer == 0) {
						std::cout << "failed to create a Eltwise layer!\n";
						return false;
					}
					if (!_add_layer_and_blobs(cur_layer, line, false))
					{
						delete cur_layer;
						return false;
					}
					layer_type_names.push_back("Eltwise");
				}
				else if (My_CNN_Layer::_my_strcmpi(&buf[0], "Input") == 0)
				{
					if (has_input_layer)
					{
						printf("Already have input layer\n");
						return false;
					}

					My_CNN_Layer* cur_layer = new ZQ_CNN_Layer_NCHWC_Input<Tensor4D>();
					if (cur_layer == 0) {
						std::cout << "failed to create a Input layer!\n";
						return false;
					}
					if (!_add_layer_and_blobs(cur_layer, line, true))
					{
						delete cur_layer;
						return false;
					}
					cur_layer->GetTopDim(input_C, input_H, input_W);
					layer_type_names.push_back("Input");
				}
				line = "";

			}
			return true;
		}

		bool _load_model_file(const std::string& file)
		{
			int layer_num = layers.size();
			if (layers.size() == 0)
				return false;
			FILE* in = 0;
#if defined(_WIN32)
			fopen_s(&in, file.c_str(), "rb");
#else
			in = fopen(file.c_str(), "rb");
#endif
			if (in == 0)
			{
				std::cout << "failed to open " << file << "\n";
				return false;
			}
			for (int i = 0; i < layer_num; i++)
			{
				if (!layers[i]->LoadBinary_NCHW(in))
				{
					fclose(in);
					std::cout << "Failed to load Binary for layer " << layers[i]->name << "\n";
					return false;
				}
			}
			fclose(in);
			return true;
		}

		bool _save_model_file(const std::string& file) const
		{
			int layer_num = layers.size();
			if (layers.size() == 0)
				return false;
			FILE* out = 0;
#if defined(_WIN32)
			fopen_s(&out, file.c_str(), "wb");
#else
			out = fopen(file.c_str(), "wb");
#endif
			if (out == 0)
			{
				std::cout << "failed to create " << file << "\n";
				return false;
			}
			for (int i = 0; i < layer_num; i++)
			{
				if (!layers[i]->SaveBinary_NCHW(out))
				{
					fclose(out);
					std::cout << "Failed to save Binary for layer " << layers[i]->name << "\n";
					return false;
				}
			}
			fclose(out);
			return true;
		}

		bool _load_model_from_buffer(const char* model_buffer, __int64 model_buffer_len)
		{
			int layer_num = layers.size();
			if (layers.size() == 0)
				return false;

			for (int i = 0; i < layer_num; i++)
			{
				__int64 readed_len_in_bytes = 0;
				if (!layers[i]->LoadBinary_NCHW(model_buffer, model_buffer_len, readed_len_in_bytes))
				{
					std::cout << "Failed to load Binary for layer " << layers[i]->name << "\n";
					return false;
				}
				model_buffer += readed_len_in_bytes;
				model_buffer_len -= readed_len_in_bytes;
			}
			return true;
		}

		bool _add_layer_and_blobs(My_CNN_Layer* cur_layer, const std::string& line, bool is_input_layer)
		{
			if (!cur_layer->ReadParam(line))
			{
				return false;
			}
			cur_layer->ignore_small_value = this->ignore_small_value;
			std::string layer_name = cur_layer->name;
			if (is_input_layer)
			{
				tops.resize(1);
				bottoms.resize(1);
				tops[0].push_back(0);
				map_name_to_blob_idx[cur_layer->top_names[0]] = 0;
				blobs.push_back(0);
				has_input_layer = true;
				layers.push_back(cur_layer);
				map_name_to_layer_idx[layer_name] = layers.size() - 1;
			}
			else
			{
				if (map_name_to_layer_idx.find(layer_name) == map_name_to_layer_idx.end())
				{
					std::vector<int> bottom_idx, top_idx;
					std::vector<std::string>& cur_bottom_names = cur_layer->bottom_names;
					for (int i = 0; i < cur_bottom_names.size(); i++)
					{
						std::map<std::string, int>::iterator name_it = map_name_to_blob_idx.find(cur_bottom_names[i]);
						if (name_it == map_name_to_blob_idx.end())
						{
							int idx = blobs.size();
							Tensor4D* blob = new Tensor4D();

							if (blob == 0)
							{
								std::cout << "failed to allocate a ZQ_CNN_Tensor4D\n";
								return false;
							}
							blobs.push_back(blob);
							bottom_idx.push_back(blobs.size() - 1);
							map_name_to_blob_idx[cur_bottom_names[i]] = idx;
						}
						else
						{
							bottom_idx.push_back(name_it->second);
						}
					}
					std::vector<std::string>& cur_top_names = cur_layer->top_names;
					for (int i = 0; i < cur_top_names.size(); i++)
					{
						std::map<std::string, int>::iterator name_it = map_name_to_blob_idx.find(cur_top_names[i]);
						if (name_it == map_name_to_blob_idx.end())
						{
							int idx = blobs.size();
							Tensor4D* blob = new Tensor4D();

							if (blob == 0)
							{
								std::cout << "failed to allocate a ZQ_CNN_Tensor4D\n";
								return false;
							}
							blobs.push_back(blob);
							top_idx.push_back(blobs.size() - 1);
							map_name_to_blob_idx[cur_top_names[i]] = idx;
						}
						else
						{
							top_idx.push_back(name_it->second);
						}
					}
					bottoms.push_back(bottom_idx);
					tops.push_back(top_idx);
					layers.push_back(cur_layer);
					map_name_to_layer_idx[layer_name] = layers.size() - 1;
				}
				else
				{
					std::cout << "There's already a layer named " << layer_name << "!\n";
					return false;
				}
			}
			return true;
		}

		bool _check_connect()
		{
			int blob_num = blobs.size();
			int layer_num = layers.size();
			if (layers.size() == 0 || blob_num == 0)
				return false;
			ZQ_CNN_Layer_NCHWC_Input<Tensor4D>* input_layer = (ZQ_CNN_Layer_NCHWC_Input<Tensor4D>*)(layers[0]);
			if (has_innerproduct_layer)
			{
				if (!input_layer->has_H_val || !input_layer->has_W_val)
				{
					std::cout << "Input dim must be specified for InnerProduct layer\n";
					return false;
				}
			}
			std::vector<bool> visited(blob_num);
			std::vector<int> blob_dim_C(blob_num);
			std::vector<int> blob_dim_H(blob_num);
			std::vector<int> blob_dim_W(blob_num);
			visited[0] = true;
			blob_dim_C[0] = input_layer->C;
			blob_dim_H[0] = input_layer->H;
			blob_dim_W[0] = input_layer->W;

			for (int i = 1; i < blob_num; i++)
				visited[i] = false;

			for (int i = 1; i < layer_num; i++)
			{
				std::vector<std::string>& bottom_names = layers[i]->bottom_names;
				int cur_bottom_c = 0;
				int cur_bottom_h = 0;
				int cur_bottom_w = 0;
				for (int j = 0; j < bottom_names.size(); j++)
				{
					std::map<std::string, int>::iterator name_it = map_name_to_blob_idx.find(bottom_names[j]);
					if (!visited[name_it->second])
					{
						std::cout << "unknown blob " << bottom_names[j] << " in Layer " << layers[i]->name << "\n";
						return false;
					}
				}
				std::vector<std::string>& top_names = layers[i]->top_names;
				for (int j = 0; j < top_names.size(); j++)
				{
					std::map<std::string, int>::iterator name_it = map_name_to_blob_idx.find(top_names[j]);
					visited[name_it->second] = true;
				}
			}


			if (!_setup())
			{
				return false;
			}
			return true;
		}

		bool _setup()
		{
			Tensor4D input;
			input.SetShape(1, input_C, input_H, input_W);
			if (map_name_to_blob_idx.size() == 0 || map_name_to_layer_idx.size() == 0 || tops.size() == 0)
				return false;

			blobs[0] = &input;

			for (int i = 0; i < layers.size(); i++)
			{
				std::vector<Tensor4D*> bottom_ptrs, top_ptrs;
				for (int j = 0; j < bottoms[i].size(); j++)
					bottom_ptrs.push_back(blobs[bottoms[i][j]]);
				for (int j = 0; j < tops[i].size(); j++)
					top_ptrs.push_back(blobs[tops[i][j]]);

				layers[i]->show_debug_info = show_debug_info;
				if (!layers[i]->LayerSetup(&bottom_ptrs, &top_ptrs))
				{
					blobs[0] = 0;
					tops[0][0] = 0;
					printf("failed to setup layer: %s\n", layers[i]->name.c_str());
					return false;
				}
			}
			blobs[0] = 0;
			tops[0][0] = 0;
			return true;
		}

		void _simplify_inplace()
		{
			for (int i = 0; i < layers.size(); i++)
			{
				if (My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), "ReLU") == 0
					|| My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), "PReLU") == 0
					|| My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), "BatchNormScale") == 0
					)
				{
					bool later_refer = false;
					for (int j = i + 1; j < layers.size(); j++)
					{
						for (int k = 0; k < bottoms[j].size(); k++)
						{
							if (bottoms[j][k] == bottoms[i][0])
							{
								later_refer = true;
								break;
							}
						}
						if (later_refer)
							break;
					}
					if (later_refer)
						continue;

					for (int j = i + 1; j < layers.size(); j++)
					{
						for (int k = 0; k < bottoms[j].size(); k++)
						{
							if (bottoms[j][k] == tops[i][0])
							{
								bottoms[j][k] = bottoms[i][0];
							}
						}
					}

					if (simplify_inplace_blob_map.find(tops[i][0]) == simplify_inplace_blob_map.end())
					{
						simplify_inplace_blob_map[tops[i][0]] = bottoms[i][0];
					}
					tops[i][0] = bottoms[i][0];
				}
			}
		}

		bool _merge_bn()
		{
			std::vector<My_CNN_Layer*> tmp_layers;
			std::vector<std::string> tmp_layer_type_names;
			std::vector<std::vector<int> > tmp_bottoms;
			std::vector<std::vector<int> > tmp_tops;
			for (int i = 0; i < layers.size(); i++)
			{
				/*BUG: merge innerproduct will lead MTCNN fail*/
				/*if (My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), "InnerProduct") == 0)
				{
					if (i + 1 < layers.size() && My_CNN_Layer::_my_strcmpi(layer_type_names[i + 1].c_str(), "BatchNormScale") == 0)
					{
						bool later_refer = false;
						for (int j = i + 2; j < layers.size(); j++)
						{
							for (int k = 0; k < bottoms[j].size(); k++)
							{
								if (bottoms[j][k] == tops[i][0])
								{
									later_refer = true;
									break;
								}
							}
							if (later_refer)
								break;
						}
						if (tops[i + 1][0] == bottoms[i + 1][0] || !later_refer)
						{
							ZQ_CNN_Layer_NCHWC_InnerProduct<Tensor4D>* conv_layer = (ZQ_CNN_Layer_NCHWC_InnerProduct<Tensor4D>*)layers[i];
							ZQ_CNN_Layer_NCHWC_BatchNormScale<Tensor4D>* bns_layer = (ZQ_CNN_Layer_NCHWC_BatchNormScale<Tensor4D>*)layers[i + 1];
							if (!_merge_bns_to_innerproduct(conv_layer, bns_layer))
								return false;

							delete bns_layer; bns_layer = 0;
							tmp_layers.push_back(conv_layer);
							tmp_layer_type_names.push_back(layer_type_names[i]);
							tmp_bottoms.push_back(bottoms[i]);
							tmp_tops.push_back(tops[i + 1]);
							i++;
							continue;
						}
					}
				}
				else */if (My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), "Convolution") == 0)
				{
					if (i + 1 < layers.size() && My_CNN_Layer::_my_strcmpi(layer_type_names[i + 1].c_str(), "BatchNormScale") == 0)
					{
						bool later_refer = false;
						for (int j = i + 2; j < layers.size(); j++)
						{
							for (int k = 0; k < bottoms[j].size(); k++)
							{
								if (bottoms[j][k] == tops[i][0])
								{
									later_refer = true;
									break;
								}
							}
							if (later_refer)
								break;
						}
						if (tops[i + 1][0] == bottoms[i + 1][0] || !later_refer)
						{
							//do merge
							ZQ_CNN_Layer_NCHWC_Convolution<Tensor4D>* conv_layer = (ZQ_CNN_Layer_NCHWC_Convolution<Tensor4D>*)layers[i];
							ZQ_CNN_Layer_NCHWC_BatchNormScale<Tensor4D>* bns_layer = (ZQ_CNN_Layer_NCHWC_BatchNormScale<Tensor4D>*)layers[i + 1];
							if (!_merge_bns_to_conv(conv_layer, bns_layer))
								return false;

							delete bns_layer; bns_layer = 0;
							tmp_layers.push_back(conv_layer);
							tmp_layer_type_names.push_back(layer_type_names[i]);
							tmp_bottoms.push_back(bottoms[i]);
							tmp_tops.push_back(tops[i + 1]);
							i++;
							continue;
						}
					}
				}
				else if (My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), "DepthwiseConvolution") == 0)
				{
					if (i + 1 < layers.size() && My_CNN_Layer::_my_strcmpi(layer_type_names[i + 1].c_str(), "BatchNormScale") == 0)
					{
						bool later_refer = false;
						for (int j = i + 2; j < layers.size(); j++)
						{
							for (int k = 0; k < bottoms[j].size(); k++)
							{
								if (bottoms[j][k] == tops[i][0])
								{
									later_refer = true;
									break;
								}
							}
							if (later_refer)
								break;
						}
						if (tops[i + 1][0] == bottoms[i + 1][0] || !later_refer)
						{
							//do merge
							ZQ_CNN_Layer_NCHWC_DepthwiseConvolution<Tensor4D>* dwconv_layer = (ZQ_CNN_Layer_NCHWC_DepthwiseConvolution<Tensor4D>*)layers[i];
							ZQ_CNN_Layer_NCHWC_BatchNormScale<Tensor4D>* bns_layer = (ZQ_CNN_Layer_NCHWC_BatchNormScale<Tensor4D>*)layers[i + 1];
							if (!_merge_bns_to_dwconv(dwconv_layer, bns_layer))
								return false;

							delete bns_layer; bns_layer = 0;
							tmp_layers.push_back(dwconv_layer);
							tmp_layer_type_names.push_back(layer_type_names[i]);
							tmp_bottoms.push_back(bottoms[i]);
							tmp_tops.push_back(tops[i + 1]);
							i++;
							continue;
						}
					}
				}

				tmp_layers.push_back(layers[i]);
				tmp_layer_type_names.push_back(layer_type_names[i]);
				tmp_bottoms.push_back(bottoms[i]);
				tmp_tops.push_back(tops[i]);
			}

			layers = tmp_layers;
			layer_type_names = tmp_layer_type_names;
			bottoms = tmp_bottoms;
			tops = tmp_tops;
			return true;
		}

		bool _merge_prelu()
		{
			std::vector<My_CNN_Layer*> tmp_layers;
			std::vector<std::string> tmp_layer_type_names;
			std::vector<std::vector<int> > tmp_bottoms;
			std::vector<std::vector<int> > tmp_tops;
			for (int i = 0; i < layers.size(); i++)
			{
				if (My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), "Convolution") == 0)
				{
					if (i + 1 < layers.size() && My_CNN_Layer::_my_strcmpi(layer_type_names[i + 1].c_str(), "PReLU") == 0)
					{
						bool later_refer = false;
						for (int j = i + 2; j < layers.size(); j++)
						{
							for (int k = 0; k < bottoms[j].size(); k++)
							{
								if (bottoms[j][k] == tops[i][0])
								{
									later_refer = true;
									break;
								}
							}
							if (later_refer)
								break;
						}
						if (tops[i + 1][0] == bottoms[i + 1][0] || !later_refer)
						{
							//do merge
							ZQ_CNN_Layer_NCHWC_Convolution<Tensor4D>* conv_layer = (ZQ_CNN_Layer_NCHWC_Convolution<Tensor4D>*)layers[i];
							ZQ_CNN_Layer_NCHWC_PReLU<Tensor4D>* prelu_layer = (ZQ_CNN_Layer_NCHWC_PReLU<Tensor4D>*)layers[i + 1];
							if (!_merge_prelu_to_conv(conv_layer, prelu_layer))
								return false;

							delete prelu_layer; prelu_layer = 0;
							tmp_layers.push_back(conv_layer);
							tmp_layer_type_names.push_back(layer_type_names[i]);
							tmp_bottoms.push_back(bottoms[i]);
							tmp_tops.push_back(tops[i + 1]);
							i++;
							continue;
						}
					}
				}
				else if (My_CNN_Layer::_my_strcmpi(layer_type_names[i].c_str(), "DepthwiseConvolution") == 0)
				{
					if (i + 1 < layers.size() && My_CNN_Layer::_my_strcmpi(layer_type_names[i + 1].c_str(), "PReLU") == 0)
					{
						bool later_refer = false;
						for (int j = i + 2; j < layers.size(); j++)
						{
							for (int k = 0; k < bottoms[j].size(); k++)
							{
								if (bottoms[j][k] == tops[i][0])
								{
									later_refer = true;
									break;
								}
							}
							if (later_refer)
								break;
						}
						if (tops[i + 1][0] == bottoms[i + 1][0] || !later_refer)
						{
							//do merge
							ZQ_CNN_Layer_NCHWC_DepthwiseConvolution<Tensor4D>* dwconv_layer = (ZQ_CNN_Layer_NCHWC_DepthwiseConvolution<Tensor4D>*)layers[i];
							ZQ_CNN_Layer_NCHWC_PReLU<Tensor4D>* prelu_layer = (ZQ_CNN_Layer_NCHWC_PReLU<Tensor4D>*)layers[i + 1];
							if (!_merge_prelu_to_dwconv(dwconv_layer, prelu_layer))
								return false;

							delete prelu_layer; prelu_layer = 0;
							tmp_layers.push_back(dwconv_layer);
							tmp_layer_type_names.push_back(layer_type_names[i]);
							tmp_bottoms.push_back(bottoms[i]);
							tmp_tops.push_back(tops[i + 1]);
							i++;
							continue;
						}
					}
				}

				tmp_layers.push_back(layers[i]);
				tmp_layer_type_names.push_back(layer_type_names[i]);
				tmp_bottoms.push_back(bottoms[i]);
				tmp_tops.push_back(tops[i]);
			}

			layers = tmp_layers;
			layer_type_names = tmp_layer_type_names;
			bottoms = tmp_bottoms;
			tops = tmp_tops;
			return true;
		}

		void _prepack()
		{
			for (int i = 0; i < layers.size(); i++)
				layers[i]->Prepack();
		}

		bool _merge_bns_to_innerproduct(ZQ_CNN_Layer_NCHWC_InnerProduct<Tensor4D>* conv_layer, ZQ_CNN_Layer_NCHWC_BatchNormScale<Tensor4D>* bns_layer)
		{
			Tensor4D* filters = conv_layer->filters;
			Tensor4D* b = bns_layer->b;
			Tensor4D* a = bns_layer->a;
			int N = filters->GetN();
			int kH = filters->GetH();
			int kW = filters->GetW();
			int kC = filters->GetC();
			int filterImageStep = filters->GetImageStep();
			int filterSliceStep = filters->GetSliceStep();
			int filterWithStep = filters->GetWidthStep();
			int align_size = filters->GetAlignSize();
			float* im_ptr = filters->GetFirstPixelPtr();
			for (int n = 0; n < N; n++, im_ptr += filterImageStep)
			{
				float b_v = (b->GetFirstPixelPtr())[n];
				float* slice_ptr = im_ptr;
				for (int c = 0; c < kC; c += align_size, slice_ptr += filterSliceStep)
				{

					float* row_ptr = slice_ptr;
					for (int h = 0; h < kH; h++, row_ptr += filterWithStep)
					{
						float* pix_ptr = row_ptr;
						for (int w = 0; w < kW; w++, pix_ptr += align_size)
						{
							for (int i = 0; i < align_size; i++)
							{
								pix_ptr[i] *= b_v;
								if (fabs(pix_ptr[i]) < this->ignore_small_value)
									pix_ptr[i] = 0;
							}
						}
					}
				}
			}

			if (conv_layer->bias == 0)
			{
				conv_layer->with_bias = true;
				conv_layer->bias = new Tensor4D();
				if (conv_layer->bias == 0 || !conv_layer->bias->ChangeSize(1, 1, 1, N, 0, 0))
					return false;
				for (int n = 0; n < N; n++)
				{
					float a_v = (a->GetFirstPixelPtr())[n];
					(conv_layer->bias->GetFirstPixelPtr())[n] = a_v;
				}
			}
			else
			{
				for (int n = 0; n < N; n++)
				{
					float b_v = (b->GetFirstPixelPtr())[n];
					float bias_v = (conv_layer->bias->GetFirstPixelPtr())[n];
					float a_v = (a->GetFirstPixelPtr())[n];
					(conv_layer->bias->GetFirstPixelPtr())[n] = bias_v*b_v + a_v;
				}
			}
			return true;
		}

		bool _merge_bns_to_conv(ZQ_CNN_Layer_NCHWC_Convolution<Tensor4D>* conv_layer, ZQ_CNN_Layer_NCHWC_BatchNormScale<Tensor4D>* bns_layer)
		{
			Tensor4D* filters = conv_layer->filters;
			Tensor4D* b = bns_layer->b;
			Tensor4D* a = bns_layer->a;
			int N = filters->GetN();
			int kH = filters->GetH();
			int kW = filters->GetW();
			int kC = filters->GetC();
			int filterImageStep = filters->GetImageStep();
			int filterSliceStep = filters->GetSliceStep();
			int filterWithStep = filters->GetWidthStep();
			int align_size = filters->GetAlignSize();
			float* im_ptr = filters->GetFirstPixelPtr();
			for (int n = 0; n < N; n++, im_ptr += filterImageStep)
			{
				float b_v = (b->GetFirstPixelPtr())[n];
				float* slice_ptr = im_ptr;
				for (int c = 0; c < kC; c += align_size, slice_ptr += filterSliceStep)
				{

					float* row_ptr = slice_ptr;
					for (int h = 0; h < kH; h++, row_ptr += filterWithStep)
					{
						float* pix_ptr = row_ptr;
						for (int w = 0; w < kW; w++, pix_ptr += align_size)
						{
							for (int i = 0; i < align_size; i++)
							{
								pix_ptr[i] *= b_v;
								if (fabs(pix_ptr[i]) < this->ignore_small_value)
									pix_ptr[i] = 0;
							}
						}
					}
				}
			}

			if (conv_layer->bias == 0)
			{
				conv_layer->with_bias = true;
				conv_layer->bias = new Tensor4D();
				if (conv_layer->bias == 0 || !conv_layer->bias->ChangeSize(1, 1, 1, N, 0, 0))
					return false;
				for (int n = 0; n < N; n++)
				{
					float a_v = (a->GetFirstPixelPtr())[n];
					(conv_layer->bias->GetFirstPixelPtr())[n] = a_v;
				}
			}
			else
			{
				for (int n = 0; n < N; n++)
				{
					float b_v = (b->GetFirstPixelPtr())[n];
					float bias_v = (conv_layer->bias->GetFirstPixelPtr())[n];
					float a_v = (a->GetFirstPixelPtr())[n];
					(conv_layer->bias->GetFirstPixelPtr())[n] = bias_v*b_v + a_v;
				}
			}
			return true;
		}

		bool _merge_prelu_to_conv(ZQ_CNN_Layer_NCHWC_Convolution<Tensor4D>* conv_layer, ZQ_CNN_Layer_NCHWC_PReLU<Tensor4D>* prelu_layer)
		{
			Tensor4D* slope = prelu_layer->slope;
			conv_layer->with_prelu = true;
			conv_layer->prelu_slope = slope;
			prelu_layer->slope = 0;
			return true;
		}

		bool _merge_bns_to_dwconv(ZQ_CNN_Layer_NCHWC_DepthwiseConvolution<Tensor4D>* dwconv_layer, ZQ_CNN_Layer_NCHWC_BatchNormScale<Tensor4D>* bns_layer)
		{
			Tensor4D* filters = dwconv_layer->filters;
			Tensor4D* b = bns_layer->b;
			Tensor4D* a = bns_layer->a;
			int kH = filters->GetH();
			int kW = filters->GetW();
			int kC = filters->GetC();
			int filterSliceStep = filters->GetSliceStep();
			int filterWidthStep = filters->GetWidthStep();
			int align_size = filters->GetAlignSize();
			float* b_data = b->GetFirstPixelPtr();
			float* slice_ptr = filters->GetFirstPixelPtr();
			for (int c = 0; c < kC; c += align_size, slice_ptr += filterSliceStep)
			{
				float* row_ptr = slice_ptr;
				for (int h = 0; h < kH; h++, row_ptr += filterWidthStep)
				{
					float* pix_ptr = row_ptr;
					for (int w = 0; w < kW; w++, pix_ptr += align_size)
					{
						for (int i = 0; i < align_size; i++)
						{
							pix_ptr[i] *= b_data[c + i];
							if (fabs(pix_ptr[i]) < this->ignore_small_value)
								pix_ptr[i] = 0;
						}
					}
				}
			}

			if (dwconv_layer->bias == 0)
			{
				dwconv_layer->with_bias = true;
				dwconv_layer->bias = new Tensor4D();
				if (dwconv_layer->bias == 0 || !dwconv_layer->bias->ChangeSize(1, 1, 1, kC, 0, 0))
					return false;
				for (int c = 0; c < kC; c++)
				{
					float a_v = (a->GetFirstPixelPtr())[c];
					(dwconv_layer->bias->GetFirstPixelPtr())[c] = a_v;
				}
			}
			else
			{
				for (int c = 0; c < kC; c++)
				{
					float b_v = (b->GetFirstPixelPtr())[c];
					float bias_v = (dwconv_layer->bias->GetFirstPixelPtr())[c];
					float a_v = (a->GetFirstPixelPtr())[c];
					(dwconv_layer->bias->GetFirstPixelPtr())[c] = bias_v*b_v + a_v;
				}
			}
			return true;
		}

		bool _merge_prelu_to_dwconv(ZQ_CNN_Layer_NCHWC_DepthwiseConvolution<Tensor4D>* dwconv_layer, ZQ_CNN_Layer_NCHWC_PReLU<Tensor4D>* prelu_layer)
		{
			Tensor4D* slope = prelu_layer->slope;
			dwconv_layer->with_prelu = true;
			dwconv_layer->prelu_slope = slope;
			prelu_layer->slope = 0;
			return true;
		}
	};
}
#endif
