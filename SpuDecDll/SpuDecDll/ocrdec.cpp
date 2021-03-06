// functions for performing ocr decode

#include "stdafx.h"
#include "roapi.h"
#include <ppltasks.h>
#include "robuffer.h"
#include <wrl.h>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "spudec.h"
#include <vlc_aout.h>
#include <ppltasks.h>
#include <agents.h>
using namespace Windows::Foundation;
using namespace Windows::System;
using namespace Windows::Media::Ocr;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::Storage::Pickers;
using namespace concurrency;
using namespace Microsoft::WRL;

// Bitmap holder of currently loaded image.
static Windows::Graphics::Imaging::SoftwareBitmap^ bitmap;

static bool DoneLoadingBitmap = false;
task<void> LoadBitmapImage(StorageFile^ file)
{	
	return create_task(file->OpenAsync(FileAccessMode::Read)).then([](IRandomAccessStream^ stream)
	{
		return BitmapDecoder::CreateAsync(stream);
	}).then([](BitmapDecoder^ decoder) -> IAsyncOperation<SoftwareBitmap^>^
	{
		return decoder->GetSoftwareBitmapAsync();
	}).then([](SoftwareBitmap^ imageBitmap)
	{
		bitmap = imageBitmap;
		DoneLoadingBitmap = true;
	});
}

void LoadSampleImage()
{
	DoneLoadingBitmap = false;
	auto getFileOp = KnownFolders::PicturesLibrary->GetFileAsync("splash-sdk.png");
	create_task(getFileOp).then([](StorageFile^ storagefile)
	{
		return LoadBitmapImage(storagefile);
	}).then([]() { });
 
	while (!DoneLoadingBitmap) {};
}

void SaveSoftwareBitmapToFile()
{
	auto getFileOp = KnownFolders::PicturesLibrary->GetFileAsync("subtitledebug.png");
	create_task(getFileOp).then([](StorageFile^ storagefile)
	{
		auto outFile = storagefile->OpenAsync(FileAccessMode::ReadWrite);
		create_task(outFile).then([](IRandomAccessStream^ stream)
		{
			// Create an encoder with the desired format
			auto encodetask = BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId, stream);
			create_task(encodetask).then([](BitmapEncoder^ encoder)
			{

				// Set the software bitmap
				encoder->SetSoftwareBitmap(bitmap);

				encoder->IsThumbnailGenerated = false;
				auto savetask = encoder->FlushAsync();
				create_task(savetask).then([]() {});
			});
		});

	});


}



static bool DllInitialized = false;
static int recogdone = 0;

//extern "C"  __declspec(dllexport) void _cdecl InitOCRDll()
void InitOCRDll() 
{
	if (!DllInitialized)
	{
		Windows::Foundation::Initialize();
		DllInitialized = true;
	}
}

static Platform::String^ MyOcrText;
const wchar_t BadTextVal[] = L"OCR engine failed to load";
MIDL_INTERFACE("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")
IMemoryBufferByteAccess : IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE GetBuffer(
		BYTE   **value,
		UINT32 *capacity
		);
};

void FillBitMap(subpicture_data_t * SubImageData, spu_properties_t * SpuProp, int croppedheight)
{
	byte * mypixeldata = nullptr;
	const uint16_t *p_source = SubImageData->p_data;
	int i_len;
	byte i_color;
	UINT32 capacity = 0;
	
	// add some height on top & bottom, seems ocr lib works a little better with more space to figure things out, so, multiply by 3 to pad top & bottom
	bitmap = ref new SoftwareBitmap(BitmapPixelFormat::Bgra8, SpuProp->i_width, (croppedheight * 3), BitmapAlphaMode::Ignore);  

	BitmapBuffer^ MyBitBuf = bitmap->LockBuffer(BitmapBufferAccessMode::ReadWrite);
	
	auto reference = MyBitBuf->CreateReference();
	ComPtr<IMemoryBufferByteAccess> bufferByteAccess;
	HRESULT result = reinterpret_cast<IInspectable*>(reference)->QueryInterface(IID_PPV_ARGS(&bufferByteAccess));

	if (result == S_OK)
	{
		result = bufferByteAccess->GetBuffer(&mypixeldata, &capacity);

		if (result == S_OK)
		{
			BitmapPlaneDescription bufferLayout = MyBitBuf->GetPlaneDescription(0);
			for (int i_y = 0; i_y < bufferLayout.Height; i_y++)
			{
				// only draw RLE part if in middle of pic area
				if ((i_y >= croppedheight) && (i_y < (bufferLayout.Height - croppedheight)))
				{
					// Draw until we reach the end of the line
					for (int i_x = 0; i_x < bufferLayout.Width; i_x += i_len)
					{
						// else...
						// Get the RLE part, then draw the line
						// color bit 0 appears to be background, then 1 is the outline.  Making both black, and others white.
						i_color = ((*p_source & 0x3) >> 1) * 0xff;
						i_len = *p_source++ >> 2;
						for (int indx = 0; indx < i_len; indx++)
						{
							mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * (i_x + indx)) + 0] = i_color;
							mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * (i_x + indx)) + 1] = i_color;
							mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * (i_x + indx)) + 2] = i_color;
							mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * (i_x + indx)) + 3] = (byte)0; // alpha ignore anyway
						}
					}
				}
				else
				{
					// if top 3rd or bottom 3rd, then draw black, this buffer space was added to help OCR
					for (int i_x = 0; i_x < bufferLayout.Width; i_x++)
					{
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 0] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 1] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 2] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 3] = (byte)0; // alpha ignore anyway
					}
				}
			}

		}

	}
}
wchar_t * OcrDecodeText(subpicture_data_t * SubImageData, spu_properties_t * SpuProp)
{
	OcrEngine^ ocrEngine = nullptr;

	InitOCRDll();
	ocrEngine = OcrEngine::TryCreateFromUserProfileLanguages();

	if (ocrEngine != nullptr)
	{
		MyOcrText = L"No Text Detected!";
		// this is cropped height from spudec
		int croppedheight = SpuProp->i_height - SubImageData->i_y_top_offset - SubImageData->i_y_bottom_offset;
		if (croppedheight > 0)
		{

			// load image
			//LoadSampleImage();
			// or, use passed in image...
			FillBitMap(SubImageData, SpuProp, croppedheight);

			// Recognize text from image.
			recogdone = 0;

			// for debug, can save image to file
			//SaveSoftwareBitmapToFile();
			auto recognizeOp = ocrEngine->RecognizeAsync(bitmap);
			create_task(recognizeOp).then([ocrEngine](OcrResult^ ocrResult)
			{
				if (ocrResult->Text)
				{
					MyOcrText = ocrResult->Text;
				}
				recogdone = 1;
			});
			while(recogdone==0)
			{
			};
		}
		return (wchar_t *)MyOcrText->Data();
	}
	else
	{
		return (wchar_t *)BadTextVal;
	}
}

// taken from MS example
task<void> complete_after(unsigned int timeout)
{
	// A task completion event that is set when a timer fires.
	task_completion_event<void> tce;

	// Create a non-repeating timer.
	auto fire_once = new timer<int>(timeout, 0, nullptr, false);
	// Create a call object that sets the completion event after the timer fires.
	auto callback = new call<int>([tce](int)
	{
		tce.set();
	});

	// Connect the timer to the callback and start the timer.
	fire_once->link_target(callback);
	fire_once->start();

	// Create a task that completes after the completion event is set.
	task<void> event_set(tce);

	// Create a continuation task that cleans up resources and
	// and return that continuation task.
	return event_set.then([callback, fire_once]()
	{
		delete callback;
		delete fire_once;
	});
}

// wait time (ms) to start mute, duration of mute, pointer to audio
void DoMute(int startmute, int Duration, audio_output_t *p_aout)
{
	if (p_aout != NULL)
	{
		// mute at start time
		task<void> unmute_task = complete_after(startmute).then([p_aout, Duration]
		{
			//vlc_object_hold(p_aout);
			aout_MuteSet(p_aout, TRUE);
			//vlc_object_release(p_aout);
			// unmute after duration
			task<void> unmute_task = complete_after(Duration).then([p_aout]
			{
				// Do the next thing, on the same thread.
				//vlc_object_hold(p_aout);
				aout_MuteSet(p_aout, FALSE);
				vlc_object_release(p_aout);
			});
		});
	}
}
