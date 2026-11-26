"""Static source contracts for frame generation and OpenXR image ownership.

This checks source contracts; the moved shaders are also compiled separately with
the Android NDK glslc. It does not execute OpenXR, a compositor, or visual smoothness.
"""
import argparse
from pathlib import Path
import re
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
REPO = Path(__file__).resolve().parents[1]
SCENE = "headset/app/src/main/cpp/gfx/scene.cpp"
APP = "headset/app/src/main/cpp/app/app.cpp"
XR = "headset/app/src/main/cpp/xr/xr_context.cpp"
XRH = "headset/app/src/main/cpp/xr/xr_context.h"
VR = "pc/streamer/src/stream/vr_stream.cpp"
ESTIMATOR = "pc/streamer/src/stream/motion_estimator.cpp"
ESTIMATOR_H = "pc/streamer/src/stream/motion_estimator.h"
FOVEATOR = "pc/streamer/src/stream/foveator.cpp"
MAILBOX = "shared/include/rc/motion_field.h"
DECODER = "headset/app/src/main/cpp/media/video_decoder.cpp"
DECODER_H = "headset/app/src/main/cpp/media/video_decoder.h"
DECODER_V4L2 = "headset/app/src/main/cpp/media/video_decoder_v4l2.cpp"
DECODER_COMMON = "headset/app/src/main/cpp/media/video_decoder_common.cpp"
SKY = "headset/app/src/main/cpp/gfx/sky.cpp"
STREAM = "headset/app/src/main/cpp/net/stream_client.cpp"
GENERATION = "shared/include/rc/frame_generation.h"
POLICY = "shared/include/rc/link_policy.h"
TOKENS = re.compile(r'R"(?P<raw>[\s\S]*?)\)"|"(?:\\.|[^"\\])*"|(?<![\w])\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]')


def mask_noncode(source, keep_strings=False):
    masked = list(source)
    for match in TOKENS.finditer(source):
        token = match[0]
        if token == "{" or token == "}":
            continue
        if keep_strings and not token.startswith(("//", "/*")):
            continue
        for index in range(match.start(), match.end()):
            if source[index] not in "\r\n":
                masked[index] = " "
    return "".join(masked)


def compact(source):
    return re.sub(r"\s+", "", mask_noncode(source, keep_strings=True))


def strip_comments(source):
    result = []
    end = 0
    for match in TOKENS.finditer(source):
        if not match[0].startswith(("//", "/*")):
            continue
        result.append(source[end:match.start()])
        end = match.end()
    result.append(source[end:])
    return "".join(result)


def glsl(source):
    return re.sub(r"\s+", "", strip_comments(source))


def block(source, signature, start=0):
    masked = mask_noncode(source)
    start = masked.index(signature, start)
    opening = masked.index("{", start)
    depth = 0
    for match in TOKENS.finditer(source, opening):
        if match[0] == "{":
            depth += 1
        elif match[0] == "}":
            depth -= 1
            if depth == 0:
                return source[start:match.end()]
    raise AssertionError(f"unclosed block: {signature}")


def shader(source, name):
    masked = mask_noncode(source)
    assignment = re.search(rf"\b{re.escape(name)}\s*=", masked)
    if not assignment:
        raise AssertionError(f"missing shader: {name}")
    raw_start = source.index('R"(', assignment.end())
    body_start = raw_start + 3
    body_end = source.index(')"', body_start)
    return source[body_start:body_end]


def audit(scene, app, xr, xrh, vr, mailbox, decoder, sky, stream, generation, policy):
    errors = []
    dec = compact(decoder)

    def ordered(body, first, second):
        return body.find(first) >= 0 and body.find(second) >= 0 and body.index(first) < body.index(second)

    def section(body, start, end=None):
        first = body.find(start)
        if first < 0:
            return ""
        if end is None:
            return body[first:]
        last = body.find(end, first + len(start))
        return body[first:] if last < 0 else body[first:last]

    def require(condition, message):
        if not condition:
            errors.append(message)

    require("GLuintTexture()const{std::lock_guardlock(m_geometryMutex);returnm_texture;}" in dec and
            "intTextureWidth()const{std::lock_guardlock(m_geometryMutex);returnm_textureWidth;}" in dec and
            "intTextureHeight()const{std::lock_guardlock(m_geometryMutex);returnm_textureHeight;}" in dec and
            "TextureGeometryGeometry()const{std::lock_guardlock(m_geometryMutex);return{m_texture,m_textureWidth,m_textureHeight,{m_crop[0],m_crop[1],m_crop[2],m_crop[3]}};}" in dec,
            "Decoder: expose actual dimensions and crop as one locked texture snapshot")
    require("PublishTextureGeometry(Texture(),int(desc.width),int(desc.height),nextCrop);" in dec and
            "std::memcpy(m_pendingCrop,nextCrop,sizeof(m_pendingCrop));" in dec and
            "PublishTextureGeometry(latched.texture,int(latched.width),int(latched.height),latched.crop);" in dec,
            "Decoder: Android publishes at bind; Linux stages the batch crop and publishes actual dimensions in Latch")
    publish_geometry = compact(block(decoder, "void VideoDecoder::PublishTextureGeometry("))
    require(ordered(publish_geometry, "std::lock_guardlock(m_geometryMutex);", "m_texture=texture;") and
            ordered(publish_geometry, "m_texture=texture;", "m_textureWidth=width;") and
            "m_textureHeight=height;" in publish_geometry and
            "std::memcpy(m_crop,crop,sizeof(m_crop));" in publish_geometry,
            "Decoder: publish texture, dimensions and crop under one geometry lock")
    reset_geometry = compact(block(decoder, "void VideoDecoder::ResetTextureGeometry("))
    require("constfloatcrop[4]={0,0,0,0};PublishTextureGeometry(0,0,0,crop);" in reset_geometry,
            "Decoder: reset texture, actual dimensions and crop through the locked publisher")
    android_init = compact(block(decoder, "bool VideoDecoder::Init("))
    android_shutdown = compact(block(decoder, "void VideoDecoder::Shutdown("))
    require("ResetTextureGeometry();" in android_init and "ResetTextureGeometry();" in android_shutdown and
            "GLuinttexture=0;glGenTextures(1,&texture);" in android_init and "if(!texture){" in android_init and
            ordered(android_init, "glTexParameteri(", "PublishTextureGeometry(texture,0,0,crop);") and
            ordered(android_shutdown, "autogeometry=Geometry();", "ResetTextureGeometry();") and
            ordered(android_shutdown, "ResetTextureGeometry();", "glDeleteTextures(1,&geometry.texture);"),
            "Decoder: Android creates, publishes, snapshots and deletes the texture through its geometry lifecycle")
    android_latch = compact(block(decoder, "bool VideoDecoder::Latch("))
    require("if(!desc.width||!desc.height){" in android_latch and
            ordered(android_latch, "if(!desc.width||!desc.height){", "pCreateImage(display") and
            ordered(android_latch, "pImageTargetTexture(GL_TEXTURE_EXTERNAL_OES,egl);",
                    "PublishTextureGeometry(Texture(),int(desc.width),int(desc.height),nextCrop);"),
            "Decoder: reject empty Android buffers before EGL import and publish geometry only after binding")
    v4l2_start = decoder.index("// Hardware video decoding on Linux")
    v4l2_init = compact(block(decoder, "bool VideoDecoder::Init(", v4l2_start))
    v4l2_setup = compact(block(decoder, "bool VideoDecoder::SetUpCapture(", v4l2_start))
    v4l2_shutdown = compact(block(decoder, "void VideoDecoder::Shutdown(", v4l2_start))
    require("GLuinttexture=0;glGenTextures(1,&texture);" in v4l2_init and "if(!texture){" in v4l2_init and
            ordered(v4l2_init, "glTexParameteri(", "PublishTextureGeometry(texture,0,0,crop);") and
            ordered(v4l2_shutdown, "autogeometry=Geometry();", "ResetTextureGeometry();") and
            ordered(v4l2_shutdown, "ResetTextureGeometry();", "glDeleteTextures(1,&geometry.texture);"),
            "Decoder: Linux creates, publishes, snapshots and deletes the texture through its geometry lifecycle")
    v4l2_latch = compact(block(decoder, "bool VideoDecoder::Latch(", v4l2_start))
    require("m_pictures.clear();m_ready.clear();m_shown=m_retiredPicture=-1;" in v4l2_setup and
            "m_captureFormat=m_captureWidth=m_captureHeight=m_captureStride=m_captureUvOffset=0;" in v4l2_setup and
            "PublishTextureGeometry(texture,0,0,zero);" in v4l2_setup and
            ordered(v4l2_setup, "constGLuinttexture=Geometry().texture;", "PublishTextureGeometry(texture,0,0,zero);"),
            "Decoder: V4L2 rollback destroys EGLImages, clears capture state but keeps the GL texture for Shutdown")
    require("std::memcpy(m_pendingCrop,nextCrop,sizeof(m_pendingCrop));" in v4l2_setup and
            ordered(v4l2_setup, "VIDIOC_STREAMON,&type)<0", "std::memcpy(m_pendingCrop,nextCrop,sizeof(m_pendingCrop));") and
            ordered(v4l2_latch, "pImageTargetTexture(GL_TEXTURE_EXTERNAL_OES,latched.image);",
                    "PublishTextureGeometry(latched.texture,int(latched.width),int(latched.height),latched.crop);") and
            "std::lock_guardlock(m_pictureMutex);" in v4l2_latch and
            "size_t(index)>=m_pictures.size()" in v4l2_latch,
            "Decoder: V4L2 stages crop at setup, publishes geometry only after Latch binds the EGLImage under lock")
    fail_body = section(v4l2_setup, "constautofail=[&]{", "v4l2_formatfmt{};")
    require("m_retiredImages.push_back(picture.image);" in fail_body and
            "m_pictures.clear();m_ready.clear();m_shown=m_retiredPicture=-1;" in fail_body and
            "pDestroyImage(eglGetCurrentDisplay(),picture.image);" not in fail_body,
            "Decoder: V4L2 fail() queues EGLImages for the GL thread instead of calling EGL on the output thread")
    require("constuint64_tgeneration=++m_batchGeneration;" in v4l2_setup and
            "picture.generation=generation;" in v4l2_setup and
            "m_pictures.assign(req.count,{});" in v4l2_setup and
            ordered(v4l2_setup, "m_pictures.assign(req.count,{});", "picture.generation=generation;"),
            "Decoder: V4L2 rebuild stamps a monotonic batch generation and retires the old batch")
    require("m_batchGeneration==latched.generation" in v4l2_latch and
            "m_pictures[size_t(index)].generation==latched.generation" in v4l2_latch and
            "pic.generation!=m_batchGeneration" in v4l2_latch and
            "DestroyRetiredImagesLocked();" in v4l2_latch,
            "Decoder: V4L2 Latch rejects stale-batch entries and guards fd-reuse ABA by generation")
    require("if(latched.generation!=m_batchGeneration)" in v4l2_latch and
            "if(latched.generation==m_batchGeneration)" in v4l2_latch and
            "++m_batchGeneration;" in fail_body and
            ordered(v4l2_latch, "if(latched.generation!=m_batchGeneration){",
                    "PublishTextureGeometry(latched.texture,int(latched.width),int(latched.height),latched.crop);") and
            ordered(v4l2_latch, "PublishTextureGeometry(latched.texture,int(latched.width),int(latched.height),latched.crop);",
                    "m_shown=index;"),
            "Decoder: V4L2 bind/publish/shown share one picture lock and re-check the batch generation; "
            "fail() invalidates in-flight Latched copies")
    require("if(display==EGL_NO_DISPLAY)" in dec,
            "Decoder: V4L2 retired-image destruction never hands EGL_NO_DISPLAY to the driver")
    mismatch_body = section(v4l2_latch, "if(latched.generation!=m_batchGeneration){",
                            "glBindTexture(GL_TEXTURE_EXTERNAL_OES,latched.texture);")
    require("DestroyRetiredImagesLocked();returnfalse;}" in mismatch_body and
            "pDestroyImage" not in mismatch_body,
            "Decoder: V4L2 generation-mismatch path leaves its EGLImage to m_retiredImages "
            "(rebuild already queued it; a second destroy is a double free)")
    require("constEGLDisplaydisplay=eglGetCurrentDisplay();"
            "if(latched.image!=EGL_NO_IMAGE_KHR&&pDestroyImage&&display!=EGL_NO_DISPLAY)"
            "pDestroyImage(display,latched.image);returnfalse;}" in v4l2_latch,
            "Decoder: V4L2 import-failure path destroys its own fresh image exactly once, "
            "with the same EGL_NO_DISPLAY guard")
    require("latched.texture=Geometry().texture;" in v4l2_latch and
            ordered(v4l2_latch, "std::lock_guardlock(m_pictureMutex);", "m_shown=index;"),
            "Decoder: V4L2 texture snapshot uses the geometry lock and shown indices are guarded")
    require("m_running=false;m_cv.notify_all();if(m_inputThread.joinable())m_inputThread.join();" in v4l2_shutdown and
            "if(m_outputThread.joinable())m_outputThread.join();" in v4l2_shutdown and
            "m_captureFormat=m_captureWidth=m_captureHeight=m_captureStride=m_captureUvOffset=0;" in v4l2_shutdown and
            "m_ready.clear();m_shown=m_retiredPicture=-1;ResetTextureGeometry();" in v4l2_shutdown,
            "Decoder: V4L2 shutdown joins failed workers and clears capture geometry")
    require("constautotexture=dec->Geometry();" in compact(app) and
            "m_scene.RememberExternal(texture.texture,texture.width,texture.height,texture.crop,foveation);" in compact(app) and
            "dec->Crop()" not in compact(app),
            "External generation: consume one actual-buffer geometry snapshot per latched frame")

    foveate = glsl(shader(scene, "kFoveateFs"))
    require("vec2Foveate(vec2uv,floateye)" in foveate and "floateye=uv.x<0.5?0.0:1.0;" not in foveate,
            "Foveate: eye identity is explicit, never inferred from a clamped UV")
    for name in ("kBlitExtFs", "kBlitExtGammaFs"):
        require("Foveate(vec2(vUv.x,1.0-vUv.y),step(0.5,vUv.x))" in glsl(shader(scene, name)),
                f"{name}: derive eye identity before foveation from the unclamped raster UV")
    require("Foveate(uv,eye)" in glsl(shader(scene, "kExternalMovedFs")),
            "kExternalMovedFs: preserve the raster eye through motion clamping and foveation")

    gsr = glsl(shader(scene, "kGsrFs"))
    # Static checks assert STRUCTURE only (the helper is called by every gather and uses
    # a max() clamp).  Numeric correctness — the .w left column, the (base+1.0) centering,
    # and per-eye clamping — is proven exclusively by test-shader-coordinate-pixels.py's
    # independent integer-space reference, not by matching this implementation text.
    require("highpvec4GatherEye(highpvec2uv,floateye){" in gsr and
            "highpfloatfootprint=uv.x*width-0.5;" in gsr and
            "highpfloatbase=floor(footprint);" in gsr and
            "base=clamp(base,lo,max(lo,hi-2.0));" in gsr and
            "max(" in gsr and
            "textureGather(uTex,vec2((base+1.0)/width,uv.y),1);" in gsr and
            "if(footprint<lo)returnvec4(g.x,g.x,g.w,g.w);" in gsr and
            "if(footprint>=hi-1.0)returnvec4(g.y,g.y,g.z,g.z);" in gsr and
            "floatstd=2.181818/max(sum,1e-5);" in gsr,
            "GSR: normalized UV to a centered gather footprint; replicate the in-eye left column via .w")
    require(gsr.count("textureGather(") == 1 and gsr.count("GatherEye(") == 5 and
            "vec4left=textureGather(" not in gsr,
            "GSR: every gather uses the eye-bounded helper")

    for name in ("kKeptMovedFs", "kExternalMovedFs"):
        body = glsl(shader(scene, name))
        require(body.count("vec2motionUv=uv*uField;") == 1 and
                body.count("texture(uMotion,motionUv).rg") == 1,
                f"{name}: normalized block-center sampling contract")
        require("uv*uField-vec2(0.5)" not in body and "uv-0.5/uField" not in body,
                f"{name}: no texel/UV offset in normalized sampling")
        require("uniformvec2uMotionHalfTexel;" in body and "uniformvec2uHalfTexel;" in body,
                f"{name}: explicit motion and colour half-texel safety uniforms")
        require("motionUv.x=clamp(motionUv.x,eye*0.5+uMotionHalfTexel.x,0.5+eye*0.5-uMotionHalfTexel.x);" in body and
                "motionUv.y=clamp(motionUv.y,uMotionHalfTexel.y,1.0-uMotionHalfTexel.y);" in body and
                ordered(body, "motionUv.y=clamp", "texture(uMotion,"),
                f"{name}: clamp motion sampling inside the current eye and image")
        require("uv.x=clamp(uv.x,eye*0.5,0.5+eye*0.5);" in body and
                "uv.y=clamp(uv.y,0.0,1.0);" in body,
                f"{name}: clamp extrapolated UVs within eye horizontal bounds and vertical [0, 1]")
        if name == "kKeptMovedFs":
            require("uv.x=clamp(uv.x,eye*0.5+uHalfTexel.x,0.5+eye*0.5-uHalfTexel.x);" in body and
                    "uv.y=clamp(uv.y,uHalfTexel.y,1.0-uHalfTexel.y);" in body and
                    ordered(body, "uv.y=clamp(uv.y,uHalfTexel.y", "texture(uTex,"),
                    "kKeptMovedFs: colour sampling cannot blend across the eye seam")
        else:
            require("vec2sampleUv=uCrop.xy+Foveate(uv,eye)*uCrop.zw;" in body and
                    "vec2cropMin=uCrop.xy+eye*0.5*uCrop.zw;" in body and
                    "floateyeMin=cropMin.x+eye*1.5*uHalfTexel.x;" in body and
                    "floateyeMax=cropMax.x-(1.0-eye)*1.5*uHalfTexel.x;" in body and
                    "floateyeMid=0.5*(cropMin.x+cropMax.x);" in body and
                    "sampleUv.x=clamp(sampleUv.x,min(eyeMin,eyeMid),max(eyeMax,eyeMid));" in body and
                    "sampleUv.y=clamp(sampleUv.y,uCrop.y,uCrop.y+uCrop.w);" in body and
                    ordered(body, "sampleUv.y=clamp", "texture(uTex,"),
                    "kExternalMovedFs: one-sided-safe seam inset and anti-reversed clamp guard")

    for name, ready, texture in (
        ("DrawKeptMoved", "m_upscalerReady", "m_gsrTexture"),
        ("DrawExternalMoved", "m_externalMovedReady", "m_externalTexture"),
    ):
        body = compact(block(scene, f"bool SceneRenderer::{name}("))
        program = f'm_{"kept" if name == "DrawKeptMoved" else "external"}Moved'
        source_width = "m_gsrWidth" if name == "DrawKeptMoved" else "m_externalWidth"
        source_height = "m_gsrHeight" if name == "DrawKeptMoved" else "m_externalHeight"
        require(f'glUniform2f({program}.Uniform("uField"),fieldU,fieldV);' in body,
                f"{name}: upload caller's field UV scale")
        require(f'glUniform2f({program}.Uniform("uMotionHalfTexel"),m_motionWidth>0?0.5f/float(m_motionWidth):0.f,m_motionHeight>0?0.5f/float(m_motionHeight):0.f);' in body,
                f"{name}: upload motion half-texel safety in normalized field UVs")
        require(f'glUniform2f({program}.Uniform("uHalfTexel"),{source_width}>0?0.5f/float({source_width}):0.f,{source_height}>0?0.5f/float({source_height}):0.f);' in body,
                f"{name}: upload source colour half-texel safety")
        require(f"if(!dstFbo||!{ready}||!{texture}||!m_motionTexture)returnfalse;" in body,
                f"{name}: reject missing FBO/program/texture")

    generate = block(app, "void App::GenerateFrame(")
    gen = compact(generate)
    require(gen.count("rc::MotionFieldExtent(") == 2, "GenerateFrame: safe field extent on both axes")
    require(gen.count("rc::MotionFieldUvScale(") == 2, "GenerateFrame: field UV scale on both axes")
    require("m_vrEyeWidth>0?uint32_t(m_vrEyeWidth)*2u:uint32_t(m_vrSwapchain.width)" in gen and
            "m_vrEyeHeight>0?uint32_t(m_vrEyeHeight):uint32_t(m_vrSwapchain.height)" in gen and
            "rc::MotionFieldUvScale(m_motionWidth,m_motionBlock,pictureW)" in gen and
            "rc::MotionFieldUvScale(m_motionHeight,m_motionBlock,pictureH)" in gen,
            "GenerateFrame: scale motion field UVs by source VR picture size, not upscaled swapchain")
    require("m_motionWidth*m_motionBlock" not in gen and "m_motionHeight*m_motionBlock" not in gen,
            "GenerateFrame: no signed dimension multiplication")
    for name in ("DrawKeptMoved", "DrawExternalMoved"):
        require(re.search(rf"m_scene\.{name}\([^;]*fieldU,fieldV\);", gen) is not None,
                f"GenerateFrame: {name} receives fieldU/fieldV")
    require("constboolreleased=m_xr.ReleaseImage(m_vrSwapchain);" in gen and
            "drew=released&&drew;" in gen,
            "GenerateFrame: release failure invalidates generated success")
    require(ordered(gen, "drew=released&&drew", "m_generatedFrames++"),
            "GenerateFrame: release checked before generated counter")
    require("++m_generationDiag.acquireFailed;m_vrHasFrame=false;" in gen and
            "if(!drew){m_vrHasFrame=false;return;}" in gen,
            "GenerateFrame: invalidate projection after acquire, draw or release failure")
    for counter in ("acquireFailed", "drawFailed", "releaseFailed", "generated"):
        require(f"++m_generationDiag.{counter};" in gen,
                f"GenerateFrame: wire {counter} diagnostic counter")
    require(ordered(gen, "m_generationDiag.last=decision;", "if(!decision.draw)"),
            "GenerateFrame: retain last decision before early rejection")
    require("++m_generationDiag.calls;" in gen and
            "if(decision.attempt)++m_generationDiag.attempts;" in gen and
            "if(decision.quotaBlocked)++m_generationDiag.quotaBlocked;" in gen,
            "GenerateFrame: count calls, attempts and quota blocks")

    latch = compact(block(app, "void App::LatchVideo("))
    require("constboolhaveMeta=info.metadata.size()>=sizeof(rc::VrFrameMetadata);" in latch,
            "LatchVideo: require current frame metadata")
    require("constboolhaveFoveation=!m_vrFoveated||info.metadata.size()>=sizeof(rc::VrFrameMetadata)+sizeof(rc::VrFoveationMetadata);" in latch,
            "LatchVideo: require current foveation metadata")
    require("rc::VrFrameMetadatanextMeta;" in latch and "nextMeta=m_vrMeta" not in latch,
            "LatchVideo: never reuse stale metadata")
    require("if(!m_xr.ReleaseImage(m_vrSwapchain)){" in latch,
            "LatchVideo: handle release failure before showing the frame")
    require(ordered(latch, "if(!m_xr.ReleaseImage(m_vrSwapchain))", "m_scene.RememberExternal("),
            "LatchVideo: remember External texture only after successful release")
    require("ExternalGenerationReady()" not in latch,
            "LatchVideo: producer must not depend on consumer readiness")
    for branch, counter in (
        ("if(!dec->Latch(info)){", "noImage"),
        ("if(!haveMeta||!haveFoveation){", "metadataMissing"),
        ("if(!m_vrSwapchain.handle){", "swapchainMissing"),
        ("if(!fbo){", "acquireFailed"),
        ("if(!m_xr.ReleaseImage(m_vrSwapchain)){", "releaseFailed"),
    ):
        branch_body = section(latch, branch, "continue;")
        require(f"++m_latchDiag.{counter};" in branch_body,
                f"LatchVideo: wire {counter} diagnostic counter")
    require("m_vrMetadataBytes=info.metadata.size();" in latch,
            "LatchVideo: report received metadata size")
    require(ordered(latch, "ReadMotion(info.metadata);", "rc::FrameGenerationSlots(") and
            ordered(latch, "rc::FrameGenerationSlots(", "m_genQuota.Arm(m_settings.frameGeneration,generationSlots);"),
            "LatchVideo: decode current motion interval before arming a fresh slot budget")
    require("m_genQuota.Arm(m_settings.frameGeneration);" not in latch,
            "LatchVideo: never fall back to the single-credit quota")
    require("slotIntervalNs=std::max(m_motionIntervalSmoothNs,double(m_motionFrameNs>0?m_motionFrameNs:0));" in latch,
            "LatchVideo: a rate drop must widen the slot budget on the frame it happens, not after the EMA")
    monitor = section(latch, "if(w->layerBacked){", "}else{")
    require(ordered(monitor, "m_monitorFresh[id]=false;", "w->videoReady=false;") and
            ordered(monitor, "w->videoReady=false;", "m_xr.AcquireImage(sc)") and
            ordered(monitor, "m_xr.ReleaseImage(sc)", "m_monitorFresh[id]=true;") and
            ordered(monitor, "m_monitorFresh[id]=true;", "w->videoReady=true;"),
            "Monitor update: freshness starts false and requires successful release")
    layers = compact(block(app, "void App::BuildLayers("))
    require("m_vrHasFrame&&m_vrSwapchain.handle&&!m_vrSwapchain.imageAcquired" in layers,
            "BuildLayers: never project a swapchain with an outstanding image")
    require("!sc.handle||sc.imageAcquired||!w->layerBacked" in layers,
            "BuildLayers: never submit a monitor swapchain with an outstanding image")
    require("!it->second.handle||it->second.imageAcquired" in layers,
            "BuildLayers: never submit a UI swapchain with an outstanding image")
    ui_layers = compact(block(app, "void App::UpdateUiLayers("))
    require("if(sc.imageAcquired&&!m_xr.ReleaseImage(sc)){w.layerCurrent=false;continue;}" in ui_layers,
            "UpdateUiLayers: retry an outstanding release before reusing a UI layer")
    require("if(!m_xr.ReleaseImage(sc)){w.layerCurrent=false;continue;}" in ui_layers,
            "UpdateUiLayers: release failure invalidates UI layer reuse")

    motion = compact(block(app, "void App::ReadMotion("))
    require("m_motionMetadataBytes=metadata.size();" in motion and "m_lastMotionHeader=h;" in motion,
            "ReadMotion: retain metadata size and last parsed header")
    require("m_motionMoves=false;m_motionConsistent=false;m_motionFrameNs=0;" in motion,
            "ReadMotion: clear previous motion validity and interval before checking packet headers")
    require("rc::DecodeVrMotionMetadata(" in motion,
            "ReadMotion: delegate header validation and decoding to shared testable helper")
    for index in (1, 2, 3):
        require(f"++m_motionFailureCounts[{index}];" in motion,
                f"ReadMotion: classify failure stage {index}")
    require("VR generation diagnostics:" in app and "motion failures short-header %u, invalid-header %u, invalid-runs %u" in app,
            "VR diagnostics: publish generation, latch and motion failure windows")
    require("VR gen zero-window trace:" in app and
            "stats.attempts > 0 && stats.generated == 0 && m_zeroSampleCount > 0" in app and
            "f=%u seq=%u ts=%llu r=%s fl=0x%x mB=%u since=%.1f sm=%.1f amt=%.2f cap=%.2f" in app,
            "VR zero-window trace: independent line, only when a window generated nothing")
    require("m_zeroSamples[m_zeroSampleCount++]" in app and "m_zeroSampleCount = 0;" in app,
            "VR zero-window trace: fixed in-place samples, no per-frame allocation")
    require("s.frameIndex = m_vrFrameIndex;" in app and
            "s.trackingTimestampNs = m_vrMeta.trackingTimestampNs;" in app and
            "s.localSeq = ++m_zeroGenFrameIndex;" in app and
            "m_vrFrameIndex = info.frameIndex;" in app,
            "VR zero-window trace: wire seq for ordering, trackingTimestampNs for cross-end alignment")
    sender_body = compact(block(vr, "void VrStream::RunSender("))
    require("VRmetadataoversize:droppedframe%u" in sender_body and
            "m_sender.RequestKeyframe();}continue;" in sender_body and
            ordered(sender_body, "s_metaOversizeWarnNs=warnNow;", "m_sender.RequestKeyframe();}continue;"),
            "VR wire guard: oversize drop requests a keyframe once per throttle window")
    require(all(f"++m_motionHeaderFailures[{index}];" in motion for index in range(5)) and
            "header width-zero %u, height-zero %u, block-zero %u, timestamp-nonadvancing %u, unknown %u" in app,
            "VR diagnostics: distinguish every invalid-header cause")
    require("m_motionGateMoving=m_motionGateMean=m_motionGateSpread=m_motionGateFar=0;" in motion and
            "m_motionGateCoherent=m_motionGateGlobal=0;" in motion,
            "ReadMotion: failed packets cannot retain the previous gate statistics")
    require("m_lastMotionFailureHeader=res.header;" in motion and
            "m_lastMotionFailureTrackingNs=m_vrMeta.trackingTimestampNs;" in motion and
            "last-header-failure track %llu prev %llu grid %ux%u block %u" in app,
            "VR diagnostics: retain the last invalid-header sample without overwriting it on success")
    require("m_generationDiag.ResetWindow();" in compact(app) and "m_latchDiag.ResetWindow();" in compact(app) and
            "m_motionFailureCounts[0]=m_motionFailureCounts[1]=m_motionFailureCounts[2]=m_motionFailureCounts[3]=0;" in compact(app) and
            "for(uint32_t&count:m_motionHeaderFailures)count=0;" in compact(app),
            "VR diagnostics: reset window counters without clearing last decision")

    pc_diag = compact(block(vr, "void VrStream::LogDiagnostics(")) if "void VrStream::LogDiagnostics(" in vr else compact(vr)
    require('RC_LOGI("rc diag 1s:' in vr and
            '"Framegeneration:%.0f/sgenerated,%.0f/sshownof%.0fHz,stream%.0ffps,%.0f%%skipped"' in compact(app),
            "Diagnostics: preserve existing periodic log prefixes and field order")
    require('RC_LOGI("rcmotion1s:' in pc_diag and "motionDelta(&rc::MotionMailboxStats::produced)" in pc_diag and
            "motionDelta(&rc::MotionMailboxStats::hits)" in pc_diag and
            "motionDelta(&rc::MotionMailboxStats::misses)" in pc_diag,
            "PC diagnostics: compare encoded fields, attachments and misses")
    require(ordered(pc_diag, "m_diagMotionRequests.exchange(0,std::memory_order_acq_rel)", "m_motionResults.Stats()"),
            "PC diagnostics: snapshot encoded count before mailbox stats")
    require("m_diagMotionRequests.fetch_add(1,std::memory_order_release);" in compact(vr),
            "PC diagnostics: count successful encoded frames")
    require("rc::PublishMotionField(m_motionResults,motionRead,motionTiming,uint16_t(motion.GridWidth()),uint16_t(motion.GridHeight()),uint16_t(MotionEstimator::kBlockSize),motionField,m_motionGridPrev,uint64_t(rc::NowNs()));" in compact(vr) and
            "ElapsedPositiveNs(now,motion.oldestEnqueuedNs)" in pc_diag and
            "ElapsedPositiveNs(now,motion.newestEnqueuedNs)" in pc_diag and
            "motion.oldestTimestampNs" not in pc_diag and "motion.newestTimestampNs" not in pc_diag,
            "PC diagnostics: mailbox age uses the producer clock, never headset endpoints")
    publish = compact(block(mailbox, "bool PublishMotionField("))
    require("if(!motionRead||!timing.frameIndex||!timing.timestampNs||!timing.intervalNs||!gridWidth||!gridHeight||!blockSize||timing.intervalNs>timing.timestampNs||field.size()!=size_t(gridWidth)*size_t(gridHeight)*2)returnfalse;" in publish and
            "mailbox.Put(timing.frameIndex,timing.timestampNs,std::move(packet),enqueuedNs);" in publish,
            "Motion mailbox: publish validates identity, geometry, interval and exact field length before mutating state")
    put = compact(block(mailbox, "void Put("))
    require("m_pending[frameIndex]={timestampNs,enqueuedNs,std::move(data)};" in put and
            "stats.oldestEnqueuedNs=m_pending.begin()->second.enqueuedNs;" in compact(mailbox) and
            "stats.newestEnqueuedNs=m_pending.rbegin()->second.enqueuedNs;" in compact(mailbox),
            "Motion mailbox: retain each endpoint's independent producer enqueue time")
    require("++m_stats.misses;" in compact(block(mailbox, "bool TakeUnboundedForTest(")),
            "Motion mailbox: count unmatched encoded endpoints")
    require(re.search(r"if\(it==m_pending\.end\(\)\)\{([^}]*)\}", compact(block(mailbox, "bool TakeUnboundedForTest("))) is not None and
            "returnfalse;" in re.search(r"if\(it==m_pending\.end\(\)\)\{([^}]*)\}", compact(block(mailbox, "bool TakeUnboundedForTest(")))[1],
            "Motion mailbox: a miss must not replace caller output")
    take = compact(block(mailbox, "bool TakeUnboundedForTest("))
    require("m_pending.find(frameIndex)" in take and
            "if(it->second.timestampNs!=timestampNs){++m_stats.invalid;returnfalse;}" in take,
            "Motion mailbox: require both frame identity and endpoint validation")

    dec_init = compact(block(decoder, "bool VideoDecoder::Init("))
    require("constmedia_status_twindowStatus=AImageReader_getWindow(m_reader,&m_window);" in dec_init and
            "if(windowStatus!=AMEDIA_OK||!m_window){" in dec_init and
            "AImageReader_delete(m_reader);" in dec_init and
            "if(!texture){" in dec_init,
            "Decoder: validate AImageReader window and GL texture allocation")
    dec_cfg = compact(block(decoder, "bool VideoDecoder::Configure("))
    require("AMediaFormat*fmt=AMediaFormat_new();if(!fmt){" in dec_cfg and
            "AMediaCodec_delete(m_codecHandle);" in dec_cfg,
            "Decoder: check AMediaFormat_new before configuring MediaCodec")
    submit = compact(block(decoder, "void VideoDecoder::Submit("))
    require("if(bitstream.empty()){" in submit and "m_needKeyframe=true;" in submit,
            "Decoder: reject empty bitstreams before they enter MediaCodec")
    input_loop = compact(block(decoder, "void VideoDecoder::InputLoop("))
    invalid_input = section(input_loop, "if(!buf||capacity==0||p.data.size()>capacity){", "continue;")
    require("AMediaCodec_queueInputBuffer(m_codecHandle,size_t(index),0,0,uint64_t(p.frameIndex),0);" in
            invalid_input and "m_needKeyframe=true;" in invalid_input,
            "Decoder: return unusable input buffers and restart keyframe recovery")
    require(ordered(input_loop, 'if(queueStatus!=AMEDIA_OK){Fail("queueInputBufferfailed"', "m_lastQueued=p.frameIndex;"),
            "Decoder: never advance the queued chain after queueInputBuffer failure")
    fatal_input = section(input_loop,
                          "if(index<0&&index!=AMEDIACODEC_INFO_TRY_AGAIN_LATER){",
                          "if(index<0){")
    require('Fail("dequeueInputBufferreturnedanerror"' in fatal_input and
            fatal_input.find("break;", fatal_input.index('Fail("dequeueInputBufferreturnedanerror"')) >= 0 and
            ordered(input_loop, "if(index<0&&index!=AMEDIACODEC_INFO_TRY_AGAIN_LATER){", "if(index<0){"),
            "Decoder: fatal input dequeue errors stop the decoder for runtime recovery")
    require("(void)eglGetError();" not in dec and "(void)glGetError();" not in dec and
            "constGLenumglError=glGetError();" not in dec,
            "Decoder: no per-frame error probing on the normal import path")

    disconnect = compact(block(app, "void App::DisconnectNow("))
    require("std::erase_if(m_monitorLayers" in disconnect and
            "returnm_xr.DestroySwapchain(entry.second);" in disconnect,
            "DisconnectNow: retain monitor swapchains whose release is outstanding")
    configure = compact(block(app, "void App::ConfigureDesktopStreams("))
    require(configure.count("if(m_xr.DestroySwapchain(m_monitorLayers[id]))m_monitorLayers.erase(id);") == 2 and
            "m_monitorFresh[id]=false;" in configure,
            "ConfigureDesktopStreams: retain failed destroys and hide stale monitor images")

    destroy = compact(block(xr, "bool XrContext::DestroySwapchain("))
    require("if(sc.imageAcquired&&!ReleaseImage(sc))returnfalse;" in destroy,
            "DestroySwapchain: retain resources while an image release is outstanding")
    require(ordered(destroy, "xrDestroySwapchain(sc.handle)", "glDeleteFramebuffers(") and
            ordered(destroy, "xrDestroySwapchain(sc.handle)", "sc={};") and
            "WarnSwapchain(\"destroyfailed\",sc,result);returnfalse;" in destroy,
            "DestroySwapchain: retain runtime and GL resources after a failed destroy")
    acquire = compact(block(xr, "GLuint XrContext::AcquireImage("))
    require("if(sc.imageAcquired&&!ReleaseImage(sc))return0;" in acquire,
            "AcquireImage: retry an outstanding release before acquiring again")
    require(ordered(acquire, "sc.imageAcquired=true;", "xrWaitSwapchainImage("),
            "AcquireImage: ownership starts at successful acquire, before wait")
    release = compact(block(xr, "bool XrContext::ReleaseImage("))
    require("if(XR_SUCCEEDED(result)){sc.imageAcquired=false;returntrue;}" in release and
            "WarnSwapchain(\"releasefailed\",sc,result);returnfalse;" in release,
            "ReleaseImage: clear ownership only after runtime success")

    end = compact(block(xr, "void XrContext::EndFrame("))
    require("RetryRetiredSwapchains();" in end,
            "EndFrame: retry any retained partial swapchain destroys")
    require("constboolacquired0=m_eyes[0].imageAcquired;" in end and
            "constboolacquired1=m_eyes[1].imageAcquired;" in end and
            "projectionReady=acquired0&&acquired1;" in end,
            "EndFrame: projection requires both acquired eye images")
    require("projectionReady=projectionReady&&released0&&released1;" in end,
            "EndFrame: projection requires both successful eye releases")
    require("if(projectionReady)layers.push_back" in end,
            "EndFrame: gate projection layer on complete ownership cycle")
    require('Projectionomitted:noRender=%llueyeLayerOff=%llunotAcquired=%llureleaseFailed=%llu' in compact(xr),
            "OpenXR diagnostics: classify omitted projections")
    require("std::stringrole=" in compact(xrh) and
            "created.role=role?role:" in compact(block(xr, "bool XrContext::CreateSwapchain(")),
            "Swapchain diagnostics: own dynamic role names")
    create = compact(block(xr, "bool XrContext::CreateSwapchain("))
    require("RetryRetiredSwapchains();" in create and
            "XrSwapchainGlcreated{};" in create and "out=std::move(created);" in create,
            "CreateSwapchain: publish runtime and GL resources only after complete setup")
    require(create.count("xrEnumerateSwapchainImages(") >= 2 and
            "if(XR_FAILED(result))returnfail(" in create and "xrDestroySwapchain(created.handle)" in create and
            "m_retiredSwapchains.push_back(std::move(created));" in create,
            "CreateSwapchain: check enumeration, roll back partial creation, and retain failed destroys")
    require("RetryRetiredSwapchains();" in compact(block(xr, "void XrContext::Shutdown(")),
            "Shutdown: retry any retained partial swapchains before destroying session")
    require("images=%zuimage=%uoutstanding=%d" in compact(xr) and "acquired=%uoutstanding=%d" not in compact(xr),
            "Swapchain diagnostics: distinguish image count/index from ownership")
    require("std::map<std::string,int64_t>lastWarnNs;" in compact(xr) and
            "conststd::stringkey=sc.role+\":\"+operation;" in compact(xr),
            "Swapchain diagnostics: rate-limit each role and operation independently")

    recover = compact(block(app, "void App::RecoverFailedDecoders("))
    require("decoder->Failed()" in recover and "retryAt=now+5'000'000'000LL;" in recover,
            "Decoder recovery: detect fatal failures and throttle rebuild attempts")
    require("replacement->Init(entry.decoder->Codec(),entry.decoder->Width(),entry.decoder->Height(),entry.decoder->Fps())" in recover and
            "m_client.ResetStream(id);" in recover and "m_client.RequestKeyframe(id);" in recover,
            "Decoder recovery: rebuild with the same format, reset transport and request a keyframe")
    vr_recover = section(recover, "m_client.RequestKeyframe(id);", "}else{")
    require("if(id==rc::kVrStreamId){" in vr_recover and
            "m_scene.ClearExternal();" in vr_recover and
            "m_vrHasFrame=" in vr_recover,
            "Decoder recovery: clear stale external VR texture alongside VR frame state")

    metadata = section(latch, "if(!haveMeta||!haveFoveation){", "continue;")
    require("++m_motionFailureCounts[1];" in metadata,
            "LatchVideo: classify missing frame/foveation metadata as short-header")
    require("m_generatedFramesWindow++;" in compact(app) and
            "m_diagFlowDisplays,m_generatedFramesWindow," in compact(app) and
            "m_diagFlowDisplays-m_diagFlowShown-int(m_generatedFramesWindow)" in compact(app) and
            "m_generatedFramesWindow=0;" in compact(app),
            "VR diagnostics: shown, repeated, generated and displays share the 1 s window")
    attach_motion = compact(block(mailbox, "bool AttachMotionField("))
    require("rc::AttachMotionField(m_config.motionField,m_motionResults,job.frameIndex,job.headsetTargetNs,frameMeta,&motionRejectedOversize);" in compact(vr) and
            "if(!motionFieldEnabled)returnfalse;" in attach_motion and
            ordered(attach_motion, "if(baseSize>wireLimit){", "mailbox.TakeIfFits(frameIndex,headsetTargetNs,wireLimit-baseSize,motion,rejectedOversize)") and
            "mailbox.TakeIfFits(frameIndex,headsetTargetNs,wireLimit-baseSize,motion,rejectedOversize)" in attach_motion,
            "PC diagnostics: disabled and oversized motion fields are not counted as attached")

    decoder_latch = compact(block(decoder, "bool VideoDecoder::Latch("))
    catchup = section(decoder_latch,
                      "elseif(now-m_behindSinceNs>500'000'000LL&&now>=m_catchUpHeldUntilNs){",
                      "if(!Failed())HandOver();")
    require('Fail("releaseOutputBufferfailed"' in catchup and "m_held.pop_front();" in catchup and
            "m_held.clear();" not in catchup and
            "if(Failed()){ReleaseImage(image,nullptr);returnfalse;}" in decoder_latch,
            "Decoder: fatal catch-up releases retain unreturned buffers and discard the acquired image")
    require("if(!hb||!pCreateImage||!pGetNativeClientBuffer||!pImageTargetTexture){" in decoder_latch,
            "Decoder: require every EGL import procedure before calling it")
    require("if(n>0)std::memcpy(" in dec,
            "Decoder: do not call memcpy for an empty bitstream")
    output_loop = compact(block(decoder, "void VideoDecoder::OutputLoop("))
    require('Fail("dequeueOutputBufferreturnedanerror"' in output_loop and
            output_loop.find("break;", output_loop.index('Fail("dequeueOutputBufferreturnedanerror"')) >= 0 and
            "AMediaFormat*f=AMediaCodec_getOutputFormat(m_codecHandle);if(!f){" in output_loop,
            "Decoder: fatal dequeue errors stop the decoder and output format change guards null")
    handover = compact(block(decoder, "void VideoDecoder::HandOver("))
    stale_drop = section(handover, "while(m_held.size()>StaleDropThreshold()){", "m_skipped++;}")
    render = handover[handover.rfind("constsize_tindex=m_held.front();"):]
    for branch, name in ((stale_drop, "stale-drop"), (render, "render")):
        require('Fail("releaseOutputBufferfailed"' in branch and
                ordered(branch, 'Fail("releaseOutputBufferfailed"', "return;") and
                ordered(branch, "return;", "m_held.pop_front();"),
                f"Decoder: retain output buffer ownership after failed {name} release")
    shutdown = compact(block(decoder, "void VideoDecoder::Shutdown("))
    require("m_running=false;m_cv.notify_all();if(m_inputThread.joinable())m_inputThread.join();" in shutdown and
            "if(m_outputThread.joinable())m_outputThread.join();" in shutdown,
            "Decoder: join worker threads even after a worker marked the decoder failed")

    require('GlCheck("skydraw")' not in compact(sky),
            "Sky rendering: no synchronous glGetError on the per-eye normal path")
    disconnect = compact(block(stream, "void StreamClient::Disconnect("))
    require("if(wasRunning||m_connection)" not in disconnect and
            "if(m_connection)LogSessionEnd(false,\"local-close\"" in disconnect,
            "Session diagnostics: local-close requires a live connection")
    run = compact(block(stream, "void StreamClient::Run("))
    remote_end = section(run, "if(c->PeerDisconnected()", "break;")
    require("m_connection.reset();" in remote_end,
            "Session diagnostics: clear remotely ended connection to prevent duplicate local-close")

    require("rc::ElapsedSinceLatchedFrameNs(" in gen,
            "GenerateFrame: measure frame age from the actual latched display slot")
    require("m_vrLatchedDisplayNs=displayNs;" in latch,
            "LatchVideo: record display slot timestamp when a frame actually latches")
    vr_run = compact(block(vr, "void VrStream::Run("))
    vr_wait = compact(block(vr, "int64_t VrStream::WaitForEncoder("))
    vr_encode = compact(block(vr, "void VrStream::RunEncoder("))
    vr_gap = section(vr_run, "if(asyncMotion&&motionReady&&lastMotionFrameIndex&&", "motionSkipPending=false;")
    require("motion.HasPendingRead()" in vr_run and "WaitForEncoder(convertAvgNs,pollPendingMotion)" in vr_run,
            "PC motion: poll pending GPU readbacks during frame wait instead of waiting for next arrival")
    require("pollMotion()" in vr_wait,
            "PC motion: keep draining pending GPU readbacks while pacing WaitForEncoder")
    require(ordered(vr_encode, "if(m_config.motionField&&job.motionQueued)WaitForMotionField(job.frameIndex,job.headsetTargetNs);",
                    "rc::AttachMotionField(m_config.motionField,m_motionResults,job.frameIndex,job.headsetTargetNs,frameMeta,&motionRejectedOversize);") and
            ordered(vr_encode, "if(job.foveated){", "rc::AttachMotionField(m_config.motionField,m_motionResults,job.frameIndex,job.headsetTargetNs,frameMeta,&motionRejectedOversize);"),
            "PC motion: sync in-flight readback and append motion after frame/foveation metadata")
    require("rc::ShouldResetMotionPrevious(fs.frameIndex,lastMotionFrameIndex,motionSkipPending)" in vr_run and
            "motion.ResetPrevious();" in vr_gap and "m_motionResults.Clear()" not in vr_gap,
            "PC motion: shared gap decision resets only previous state and preserves the mailbox")
    require(ordered(vr_run, "motionPreviousTimestamp=fs.headsetTargetNs;", "if(!converted){") and
            "motionQueued=true;" in vr_run and "job.motionQueued=motionQueued;" in vr_run,
            "PC motion: advance source timing before conversion failure and wait only for queued readbacks")
    require("constboolpublishedMotion=rc::PublishMotionField(" in vr_run and
            "m_motionSyncCv.notify_all();" in section(vr_run, "if(publishedMotion){", "}"),
            "PC motion: direct readback publication wakes a waiting encoder")
    motion_gap = compact(block(mailbox, "bool ShouldResetMotionPrevious("))
    require("if(!previousFrameIndex)returnfalse;" in motion_gap and
            "if(currentFrameIndex<=previousFrameIndex)returntrue;" in motion_gap and
            "currentFrameIndex-previousFrameIndex!=1&&" in motion_gap and
            "!(deliberateSkipPending&&currentFrameIndex-previousFrameIndex==2)" in motion_gap,
            "Motion gap: only consecutive frames or one deliberate two-frame skip retain previous state")
    motion_log_policy = compact(block(mailbox, "bool ShouldLogMotionDiagnostics("))
    motion_activity = compact(block(mailbox, "bool MotionMailboxHasActivity("))
    require("returnenabled||hasMailboxActivity||!stateKnown||enabled!=previousEnabled;" in motion_log_policy,
            "Motion diagnostics: preserve enabled-periodic, first, activity and transition observations")
    require("current.pending||" in motion_activity and
            all(f"current.{name}!=previous.{name}" in motion_activity for name in
                ("produced", "hits", "misses", "expired", "evicted", "cleared", "invalid")),
            "Motion diagnostics: activity must include pending state and every mailbox counter")
    vr_flow = compact(block(vr, "void VrStream::LogFrameFlow("))
    require(vr_flow.count("VisitDiagRing(") == 2 and
            "VisitDiagRing(end,count,uint32_t(kDiagRing)" in vr_flow and
            "VisitDiagRing(sendWaitEnd,sendWaitCount,uint32_t(kDiagRing)" in vr_flow,
            "LogFrameFlow: iterate diagnostic rings via wrap-safe VisitDiagRing")
    require("ElapsedPositiveNs(now,motion.oldestEnqueuedNs)" in vr_flow and
            "ElapsedPositiveNs(now,motion.newestEnqueuedNs)" in vr_flow,
            "LogFrameFlow: compute mailbox age via 64-bit integer ElapsedPositiveNs")
    require("BoundedConvertedCount(given,bridge)" in vr_flow and
            "given-bridge" not in vr_flow,
            "LogFrameFlow: guard converted frame count against uint32_t underflow")
    motion_log = section(vr_flow, "constuint32_tmotionRequests", "m_diagJobRepl=0;")
    motion_decision = "if(rc::ShouldLogMotionDiagnostics(m_config.motionField,hasMailboxActivity," \
                      "m_motionDiagStateKnown,m_motionDiagEnabled)){"
    require("constboolhasMailboxActivity=rc::MotionMailboxHasActivity(motion,m_diagMotionPrev);" in motion_log and
            motion_decision in motion_log and "motionRequests" not in motion_decision and
            ordered(motion_log, motion_decision, 'RC_LOGI("rcmotion1s:') and
            ordered(motion_log, 'RC_LOGI("rcmotion1s:', "m_motionDiagStateKnown=true;") and
            ordered(motion_log, "m_motionDiagStateKnown=true;", "m_motionDiagEnabled=m_config.motionField;") and
            motion_log.count('RC_LOGI("rcmotion1s:') == 1,
            "Motion diagnostics: gate the unchanged log on mailbox activity, never encoded request count")
    require("m_diagMotionRequests.exchange(0,std::memory_order_acq_rel);" in motion_log and
            "m_motionDiagEnabled=m_config.motionField;m_diagMotionPrev=motion;" in motion_log,
            "Motion diagnostics: advance counters and baselines even when a quiet window is suppressed")
    interval = compact(block(mailbox, "uint64_t ResolveMotionIntervalNs("))
    require("kMinMotionIntervalNs=4'000'000ULL;" in compact(mailbox) and
            "kMaxMotionIntervalNs=200'000'000ULL;" in compact(mailbox) and
            "constboolvalidLast=lastIntervalNs>=kMinMotionIntervalNs&&lastIntervalNs<=kMaxMotionIntervalNs;" in interval and
            "if(delta>=kMinMotionIntervalNs&&delta<=kMaxMotionIntervalNs)returndelta;" in interval and
            "if(validLast)returnlastIntervalNs;" in interval and
            "returnstd::clamp(delta,kMinMotionIntervalNs,kMaxMotionIntervalNs);" in interval and
            "returnstd::clamp(uint64_t(currentSubmitNs-previousSubmitNs),kMinMotionIntervalNs,kMaxMotionIntervalNs);" in interval,
            "PC motion: reject or clamp implausible periods and ignore an invalid previous interval")
    smooth = compact(block(generation, "double SmoothMotionInterval("))
    require("double(currentNs)<double(kMinMotionIntervalNs)" in smooth and
            "double(currentNs)>double(kMaxMotionIntervalNs)" in smooth and
            "returnpreviousNs;" in smooth and
            "previousNs>=double(kMinMotionIntervalNs)&&previousNs<=double(kMaxMotionIntervalNs)" in smooth,
            "Headset motion: anomalous samples cannot enter or pollute the interval EMA")
    require("rc::ResolveMotionIntervalNs(" in vr_run,
            "PC motion: resolve shared or rewound tracking targets without inventing a shorter period")
    gen = compact(generation)
    require("latchedDisplayNs>0?displayNs-latchedDisplayNs:displayNs-trackingTimestampNs;" in gen,
            "Generation timing: prefer latched display time over latency prediction offset")
    require(ordered(gen, "out.cap=float(std::clamp(", "if(!quota.HasCredit())"),
            "Generation diagnostics: publish cap for quota and early reject decisions")
    require(gen.count("quota.Spend();") == 1 and
            gen.count("if(out.reason!=GenerationRejectReason::None)returnout;") == 2 and
            gen.rfind("if(out.reason!=GenerationRejectReason::None)returnout;") < gen.find("quota.Spend();") and
            ordered(gen, "quota.Spend();", "out.draw=true;"),
            "Generation quota: rejected attempts preserve credit; only a draw spends it")
    require("elseif(amount>out.cap)" in gen,
            "Generation diagnostics: amount rejection must use the published decision cap")
    quota = compact(block(policy, "struct FrameGenerationQuota"))
    require("intcredit=0;" in quota and "boolHasCredit()const{returncredit>0;}" in quota and
            "credit=enabled?std::clamp(count,0,4):0;" in quota and "--credit;" in quota,
            "Generation quota: bounded integer budget resets on every decoded frame")
    require("std::max(credit,count)" not in quota,
            "Generation quota: unused credits must not accumulate across decoded frames")
    return errors


def audit_estimator(estimator):
    errors = []

    def require(condition, message):
        if not condition:
            errors.append(message)

    expected = compact(shader(estimator, "kExpected"))
    coarse = compact(shader(estimator, "kCoarseCs"))
    refine = compact(shader(estimator, "kRefineCs"))
    median = compact(shader(estimator, "kMedianCs"))
    require("boolSameEye(int2blockOrigin,int2corner,int2offset,intblockW,floatscale)" in expected and
            expected.count("constintleft=blockOrigin.x<eyeW?0:eyeW;") == 2,
            "Motion GPU: candidate eye membership comes from the current block, not the shifted window")
    for name, body in (("coarse", coarse), ("refine", refine)):
        require("if(id.x>=uint(size.x/32.0)||id.y>=uint(size.y/32.0))return;" in body,
                f"Motion GPU: {name} dispatch guards padded threads")
    require("constintleft=id.x<eyeBlocks?0:int(eyeBlocks)*2;" in coarse and
            "clamp(int(id.x)*2-1,left,left+int(eyeBlocks)*2-4)" in coarse and
            "CoarseSad(int2(id.xy)*2,corner,o,base)" in coarse and
            "p.x<left||p.x>=right||q.x<left||q.x>=right" in coarse and
            "count>=4?sad*(16.0/float(count))+0.5*(abs(offset.x-base.x)+abs(offset.y-base.y)):1e9" in coarse,
            "Motion GPU: coarse boundary windows stay inside their own eye and compare mean error")
    require("int2n=int2(id.xy)+near[i];" in refine and
            "n.x<0||n.y<0||n.x>=int(size.x/32.0)||n.y>=int(size.y/32.0)" in refine and
            "(n.x<int(eyeBlocks))!=(int(id.x)<int(eyeBlocks))" in refine and
            "SameEye(int2(id.xy)*8,corner,c,8,4.0)" in refine,
            "Motion GPU: refine neighbours are in bounds and cannot cross the eye seam")
    require("uintdimX,dimY;src.GetDimensions(dimX,dimY);" in median and
            "if(id.x>=dimX||id.y>=dimY)return;" in median and
            "int2n=clamp(int2(id.xy)+int2(i-1,j-1),int2(0,0),int2(int(dimX),int(dimY))-1);" in median and
            "(n.x<int(dimX/2))!=(id.x<dimX/2)" in median,
            "Motion GPU: median replicates boundaries without mixing eyes")
    require("if(exponent==31)" in compact(estimator) and "std::bit_cast<float>" in compact(estimator),
            "Motion GPU: FP16 exponent 31 decodes as NaN/Inf instead of a huge finite value")
    return errors


def audit_foveator(foveator):
    errors = []

    def require(condition, message):
        if not condition:
            errors.append(message)

    body = glsl(shader(foveator, "kShader"))
    require("float2EyeUv(float2p,floateye)" in body and
            "clamp(p.x,eye*0.5+0.5*texel.x,eye*0.5+0.5-0.5*texel.x)" in body,
            "PC Foveator: every sample coordinate stays within its own eye's half-texel bounds")
    require("float2uv=EyeUv(float2((ux+eye)*0.5,uy),eye);" in body,
            "PC Foveator: clamp the centre before filtering")
    for offset in ("-o.x,-o.y", "o.x,-o.y", "-o.x,o.y", "o.x,o.y"):
        require(f"EyeUv(uv+float2({offset}),eye)" in body,
                f"PC Foveator: independently clamp the {offset} filtering tap")
    return errors


def self_test(scene, app, xr, xrh, vr, mailbox, decoder, sky, stream, generation, policy, estimator, foveator):
    if "m_needKeyframe=true;" in compact("int x; // m_needKeyframe=true;"):
        raise AssertionError("comment text satisfied a source contract")
    commented_signature = '/* void App::GenerateFrame() { } */\nvoid App::GenerateFrame() { return; }'
    if block(commented_signature, "void App::GenerateFrame(") != "void App::GenerateFrame() { return; }":
        raise AssertionError("a commented function satisfied block extraction")
    string_signature = 'const char* s = "void App::GenerateFrame() { }";\nvoid App::GenerateFrame() { return; }'
    if block(string_signature, "void App::GenerateFrame(") != "void App::GenerateFrame() { return; }":
        raise AssertionError("a string literal satisfied block extraction")
    shader_fixture = ('/* static const char* kExternalMovedFs = R"(fake)" ; */\n'
                      'static const char* kExternalMovedFs = R"(real)" ;')
    if shader(shader_fixture, "kExternalMovedFs") != "real":
        raise AssertionError("a commented shader satisfied shader extraction")
    if "texture(uMotion,uv*uField).rg" in glsl("// texture(uMotion, uv * uField).rg\n"):
        raise AssertionError("a shader comment satisfied a source contract")
    mutations = (
        (scene, "vec2 motionUv = uv * uField;", "vec2 motionUv = uv * uField - vec2(0.5);", "shader sampling"),
        (app, "rc::MotionFieldUvScale(m_motionWidth, m_motionBlock, pictureW)", "1.f", "field uniform caller"),
        (app, "const uint32_t pictureW = m_vrEyeWidth > 0 ? uint32_t(m_vrEyeWidth) * 2u : uint32_t(m_vrSwapchain.width);", "const uint32_t pictureW = uint32_t(m_vrSwapchain.width);", "upscaled swapchain UV scale regression"),
        (app, "rc::VrFrameMetadata nextMeta;", "rc::VrFrameMetadata nextMeta = m_vrMeta;", "stale metadata"),
        (app, "drew = released && drew;", "m_xr.ReleaseImage(m_vrSwapchain);", "release success accounting"),
        (app, "if (!released) ++m_generationDiag.releaseFailed;", "", "release failure diagnostics"),
        (app, "++m_latchDiag.noImage;", "", "latch failure diagnostics"),
        (app, 'RC_LOGI("Frame generation:', 'RC_LOGI("Generation:', "legacy log prefix"),
        (vr, "motionDelta(&rc::MotionMailboxStats::hits)", "uint64_t(0)", "motion attachment diagnostics"),
        (mailbox, "++m_stats.misses;", "", "mailbox miss accounting"),
        (decoder, "if (!buf || capacity == 0 || p.data.size() > capacity) {", "if (!buf || capacity == 0) {", "oversized input rejection"),
        (xr, "sc.imageAcquired = false;\n        return true;", "return true;", "release ownership"),
        (xr, "if (projectionReady) layers.push_back", "layers.push_back", "single-eye projection"),
        (xr, "WarnSwapchain(\"destroy failed\", sc, result);\n        return false;", "WarnSwapchain(\"destroy failed\", sc, result);\n        sc = {};\n        return true;", "failed destroy retention"),
        (xr, 'const std::string key = sc.role + ":" + operation;', 'const std::string key = "swapchain";', "independent swapchain warning limits"),
        (app, "m_generatedFramesWindow++;", "", "generated window accounting"),
        (app, "++m_motionPacketFailures;\n                    ++m_motionFailureCounts[1];", "++m_motionPacketFailures;", "short metadata failure classification"),
        (vr, "rc::AttachMotionField(m_config.motionField, m_motionResults, job.frameIndex, job.headsetTargetNs, frameMeta,\n                              &motionRejectedOversize);", "rc::AttachMotionField(true, m_motionResults, job.frameIndex, job.headsetTargetNs, frameMeta,\n                              &motionRejectedOversize);", "disabled motion mailbox lookup"),
        (mailbox, "if (!motionFieldEnabled) return false;", "if (false) return false;", "motion attachment enable gate"),
        (mailbox, "mailbox.TakeIfFits(frameIndex, headsetTargetNs, wireLimit - baseSize, motion, rejectedOversize)", "mailbox.TakeUnboundedForTest(frameIndex, headsetTargetNs, motion)", "oversize preflight before mailbox consume"),
        (mailbox, "if (baseSize > wireLimit)", "if (false)", "base metadata wire limit guard"),
        (vr, "m_motionGridPrev, uint64_t(rc::NowNs()));", "m_motionGridPrev, uint64_t(0));", "motion publish producer clock"),
        (decoder, "const media_status_t windowStatus = AImageReader_getWindow(m_reader, &m_window);\n    if (windowStatus != AMEDIA_OK || !m_window) {", "AImageReader_getWindow(m_reader, &m_window);\n    if (false) {", "image reader window guard"),
        (decoder, "AMediaFormat* fmt = AMediaFormat_new();\n    if (!fmt) {", "AMediaFormat* fmt = AMediaFormat_new();\n    if (false) {", "media format alloc guard"),
        (decoder, "AMediaFormat* f = AMediaCodec_getOutputFormat(m_codecHandle);\n            if (!f) {", "AMediaFormat* f = AMediaCodec_getOutputFormat(m_codecHandle);\n            if (false) {", "output format null guard"),
        (xr, "m_retiredSwapchains.push_back(std::move(created));", "", "partial swapchain destroy retention"),
        (decoder, "if (!hb || !pCreateImage || !pGetNativeClientBuffer || !pImageTargetTexture)", "if (!hb || !pCreateImage)", "complete EGL import guard"),
        (decoder, "AMediaCodec_queueInputBuffer(m_codecHandle, size_t(index), 0, 0, uint64_t(p.frameIndex), 0);", "(void)index;", "input buffer ownership return"),
        (decoder, "if (n > 0) std::memcpy(buf, p.data.data(), n);", "std::memcpy(buf, p.data.data(), n);", "empty bitstream memcpy guard"),
        (sky, "    glBindVertexArray(0);\n}", "    glBindVertexArray(0);\n    GlCheck(\"sky draw\");\n}", "per-eye synchronous GL error probe"),
        (stream, "if (m_connection)\n        LogSessionEnd(false, \"local-close\"", "if (wasRunning || m_connection)\n        LogSessionEnd(false, \"local-close\"", "duplicate local-close suppression"),
        (stream, '            m_state = State::Failed;\n            m_connection.reset();\n            break;', '            m_state = State::Failed;\n            break;', "remote session connection cleanup"),
        (generation, "    out.cap = float(std::clamp(in.motionIntervalSmoothNs > 0 ? in.motionIntervalSmoothNs / displayNs + 0.3 : 1.2, 1.0, 2.5));\n", "", "early decision diagnostic cap"),
        (generation, "else if (amount > out.cap)", "else if (amount > cap)", "decision cap consumer"),
        (app, "++m_generationDiag.acquireFailed;\n        m_vrHasFrame = false;", "++m_generationDiag.acquireFailed;", "failed generation projection invalidation"),
        (app, "if (!drew) {\n        m_vrHasFrame = false;\n        return;\n    }", "if (!drew) return;", "draw or release failure projection invalidation"),
        (app, "                    m_monitorFresh[id] = false;\n", "", "monitor freshness invalidation"),
        (app, "                    w->videoReady = false;\n", "", "monitor readiness invalidation"),
        (app, "                            w->videoReady = true;\n", "", "monitor readiness success accounting"),
        (app, "m_vrHasFrame && m_vrSwapchain.handle && !m_vrSwapchain.imageAcquired", "m_vrHasFrame && m_vrSwapchain.handle", "outstanding projection submission guard"),
        (app, "!sc.handle || sc.imageAcquired || !w->layerBacked", "!sc.handle || !w->layerBacked", "outstanding monitor submission guard"),
        (vr, "ElapsedPositiveNs(now, motion.oldestEnqueuedNs)", "ElapsedPositiveNs(now, motion.oldestTimestampNs)", "mailbox age clock domain"),
        (mailbox, "m_pending[frameIndex] = {timestampNs, enqueuedNs, std::move(data)};", "m_pending[timestampNs] = {timestampNs, enqueuedNs, std::move(data)};", "frame identity mailbox key"),
        (decoder, "if (bitstream.empty()) {", "if (false) {", "empty bitstream rejection"),
        (decoder, "            DestroyRetiredImagesLocked();\n            return false;\n        }\n        glBindTexture(GL_TEXTURE_EXTERNAL_OES, latched.texture);",
                  "            DestroyRetiredImagesLocked();\n            pDestroyImage(eglGetCurrentDisplay(), latched.image);\n            return false;\n        }\n        glBindTexture(GL_TEXTURE_EXTERNAL_OES, latched.texture);",
                  "generation mismatch double-frees a queued EGLImage"),
        (decoder, "            if (latched.image != EGL_NO_IMAGE_KHR && pDestroyImage && display != EGL_NO_DISPLAY)\n                pDestroyImage(display, latched.image);",
                  "            if (latched.image != EGL_NO_IMAGE_KHR && pDestroyImage)\n                pDestroyImage(eglGetCurrentDisplay(), latched.image);",
                  "import failure passes EGL_NO_DISPLAY to eglDestroyImageKHR"),
        (decoder, 'Fail("queueInputBuffer failed",', 'Warn("queueInputBuffer failed",', "fatal input queue failure"),
        (decoder, 'Fail("dequeueInputBuffer returned an error",', 'Warn("dequeueInputBuffer returned an error",', "fatal input dequeue failure"),
        (decoder, 'Fail("dequeueOutputBuffer returned an error",', 'Warn("dequeueOutputBuffer returned an error",', "fatal output dequeue failure"),
        (decoder, "        return;\n    }\n    m_held.pop_front();\n    m_inReaderSinceNs = now;", "    }\n    m_held.pop_front();\n    m_inReaderSinceNs = now;", "failed output release ownership"),
        (decoder, "                if (releaseStatus != AMEDIA_OK) {\n                    Fail(\"releaseOutputBuffer failed\",\n                         rc::StrFormat(\"status=%d index=%zu render=0 dropped=%zu held=%zu\",", "                if (releaseStatus != AMEDIA_OK) {\n                    Warn(\"releaseOutputBuffer failed\",\n                         rc::StrFormat(\"status=%d index=%zu render=0 dropped=%zu held=%zu\",", "fatal catch-up output release failure"),
        (decoder, "    if (Failed()) {\n        ReleaseImage(image, nullptr);\n        return false;\n    }", "", "failed decoder acquired-image cleanup"),
        (app, "if (sc.imageAcquired && !m_xr.ReleaseImage(sc)) {", "if (false) {", "UI outstanding release retry"),
        (app, "if (!m_xr.ReleaseImage(sc)) {\n            w.layerCurrent = false;", "if (!m_xr.ReleaseImage(sc)) {", "UI release failure invalidation"),
        (app, "!it->second.handle || it->second.imageAcquired", "!it->second.handle", "outstanding UI submission guard"),
        (xr, "out = std::move(created);", "out = created;", "atomic swapchain publication"),
        (app, "m_client.RequestKeyframe(id);", "(void)id;", "decoder recovery keyframe request"),
        (app, "m_diagFlowDisplays - m_diagFlowShown - int(m_generatedFramesWindow)", "m_diagFlowDisplays - m_diagFlowShown", "one-second repeated classification"),
        (app, "void App::ReadMotion(const std::vector<uint8_t>& metadata) {\n    m_motionMoves = false;\n    m_motionConsistent = false;\n    m_motionFrameNs = 0;", "void App::ReadMotion(const std::vector<uint8_t>& metadata) {\n    m_motionMoves = false;", "stale motion consistency across failed packets"),
        (app, "in.sinceFrameNs = rc::ElapsedSinceLatchedFrameNs(m_xr.ToMonotonicNs(frame.predictedDisplayTime), m_vrLatchedDisplayNs,\n                                                     int64_t(m_vrMeta.trackingTimestampNs));", "in.sinceFrameNs = m_xr.ToMonotonicNs(frame.predictedDisplayTime) - int64_t(m_vrMeta.trackingTimestampNs);", "predicted latency skew in frame age"),
        (app, "m_vrLatchedDisplayNs = displayNs;", "m_vrLatchedDisplayNs = 0;", "latched display slot timestamp recording"),
        (vr, "frameHoldNs = WaitForEncoder(convertAvgNs, pollPendingMotion);", "frameHoldNs = WaitForEncoder(convertAvgNs);", "async motion polling during encoder wait"),
        (vr, "if (m_config.motionField && job.motionQueued) WaitForMotionField(job.frameIndex, job.headsetTargetNs);", "if (false) WaitForMotionField(job.frameIndex, job.headsetTargetNs);", "encoder motion readback wait"),
        (vr, "rc::ShouldResetMotionPrevious(fs.frameIndex, lastMotionFrameIndex, motionSkipPending)", "rc::ShouldResetMotionPrevious(fs.frameIndex, lastMotionFrameIndex, false)", "deliberate half-rate gap signal"),
        (vr, "motion.ResetPrevious();\n            lastMotionFrameIndex = 0;", "motion.Reset();\n            lastMotionFrameIndex = 0;\n            m_motionResults.Clear();", "transient bridge skip preserves mailbox"),
        (mailbox, "if (!previousFrameIndex) return false;", "if (!previousFrameIndex) return true;", "first motion frame reset"),
        (mailbox, "currentFrameIndex - previousFrameIndex != 1 &&", "false &&", "consecutive motion frames"),
        (mailbox, "!(deliberateSkipPending && currentFrameIndex - previousFrameIndex == 2)", "(deliberateSkipPending && currentFrameIndex - previousFrameIndex == 2)", "two-frame gap allowance inversion"),
        (vr, "rc::AttachMotionField(m_config.motionField, m_motionResults, job.frameIndex, job.headsetTargetNs, frameMeta,\n                              &motionRejectedOversize);", "rc::AttachMotionField(m_config.motionField, m_motionResults, job.headsetTargetNs, job.headsetTargetNs, frameMeta,\n                              &motionRejectedOversize);", "motion frame identity matching"),
        (vr, "motionIntervalNs = rc::ResolveMotionIntervalNs(fs.headsetTargetNs, motionPreviousTimestamp,\n                                                               fs.submitPcNs, motionPrevSubmitNs, motionLastIntervalNs);", "motionIntervalNs = motionPreviousTimestamp && fs.headsetTargetNs > motionPreviousTimestamp ? fs.headsetTargetNs - motionPreviousTimestamp : 0;", "shared headset target fallback interval"),
        (vr, "motionPreviousTimestamp = fs.headsetTargetNs;", "motionPreviousTimestamp = 0;", "motion source timing across conversion failure"),
        (vr, "motionQueued = true;", "motionQueued = false;", "queued motion readback job flag"),
        (vr, "if (publishedMotion) {\n                std::lock_guard lock(m_motionSyncMutex);\n                m_motionSyncCv.notify_all();\n            }", "", "direct motion publication encoder wake"),
        (app, "rc::DecodeVrMotionMetadata(true, m_vrFoveated, m_vrMeta.trackingTimestampNs, metadata, state);", "rc::MotionDecodeResult{};\n    (void)state;", "ReadMotion calls shared decoder"),
        (app, "if (res.headerFailure & rc::kMotionHeaderZeroWidth) ++m_motionHeaderFailures[0];", "", "invalid-header width diagnostics"),
        (generation, "return latchedDisplayNs > 0 ? displayNs - latchedDisplayNs : displayNs - trackingTimestampNs;", "return displayNs - trackingTimestampNs;", "elapsed time fallback to tracking target"),
        (generation, "if (!quota.HasCredit())", "if (!quota.Spend())", "quota check without spending"),
        (generation, "    quota.Spend();\n    const float perPixel", "    const float perPixel", "draw-path quota accounting"),
        (policy, "credit = enabled ? std::clamp(count, 0, 4) : 0;", "credit = enabled ? std::clamp(std::max(credit, count), 0, 4) : 0;", "cross-frame quota accumulation"),
        (policy, "int credit = 0;", "bool credit = false;", "single-credit quota regression"),
        (app, "const int generationSlots = rc::FrameGenerationSlots(slotIntervalNs, double(frame.period));", "const int generationSlots = 1;", "dynamic display-slot budget"),
        (app, "ReadMotion(info.metadata);\n            const double slotIntervalNs", "const double slotIntervalNs", "current motion interval before quota arming"),
        (app, "std::max(m_motionIntervalSmoothNs, double(m_motionFrameNs > 0 ? m_motionFrameNs : 0))", "m_motionIntervalSmoothNs", "quota slots lagging a rate drop behind the EMA"),
        (vr, "if (rc::ShouldLogMotionDiagnostics(m_config.motionField, hasMailboxActivity,\n                                          m_motionDiagStateKnown, m_motionDiagEnabled)) {", "if (true) {", "unconditional disabled motion logging"),
        (vr, "const bool hasMailboxActivity = rc::MotionMailboxHasActivity(motion, m_diagMotionPrev);", "const bool hasMailboxActivity = motionRequests != 0;", "encoded requests mistaken for motion activity"),
        (vr, "m_diagMotionPrev = motion;", "if (hasMailboxActivity) m_diagMotionPrev = motion;", "skipped diagnostic window baseline"),
        (vr, "m_motionDiagStateKnown = true;\n        m_motionDiagEnabled = m_config.motionField;", "", "motion diagnostic transition tracking"),
        (vr, "                m_sender.RequestKeyframe();\n            }\n            continue;", "            }\n            continue;", "oversize drop must request a keyframe"),
        (mailbox, "return enabled || hasMailboxActivity || !stateKnown || enabled != previousEnabled;", "return enabled || hasMailboxActivity || enabled != previousEnabled;", "first disabled motion observation"),
        (mailbox, "return current.pending || current.produced", "return false || current.produced", "pending motion mailbox diagnostic activity"),
        (scene, "    uv.y = clamp(uv.y, uHalfTexel.y, 1.0 - uHalfTexel.y);\n    vec3 g = texture(uTex, vec2(uv.x, 1.0 - uv.y)).rgb;", "    vec3 g = texture(uTex, vec2(uv.x, 1.0 - uv.y)).rgb;", "kKeptMovedFs half-texel vertical clamp"),
        (scene, "    motionUv.x = clamp(motionUv.x, eye * 0.5 + uMotionHalfTexel.x, 0.5 + eye * 0.5 - uMotionHalfTexel.x);", "    motionUv.x = clamp(motionUv.x, eye * 0.5, 0.5 + eye * 0.5);", "kKeptMovedFs motion sampling eye seam safety"),
        (scene, "    motionUv.x = clamp(motionUv.x, eye * 0.5 + uMotionHalfTexel.x, 0.5 + eye * 0.5 - uMotionHalfTexel.x);", "    motionUv.x = clamp(motionUv.x, eye * 0.5, 0.5 + eye * 0.5);", "kExternalMovedFs motion sampling eye seam safety", 1),
        (scene, "    float eyeMin = cropMin.x + eye * 1.5 * uHalfTexel.x;", "    float eyeMin = max(cropMin.x, eye * 0.5 + uHalfTexel.x);", "external colour sampling uses the crop-space eye seam"),
        (scene, "    float eyeMax = cropMax.x - (1.0 - eye) * 1.5 * uHalfTexel.x;", "    float eyeMax = min(cropMax.x, 0.5 + eye * 0.5 - uHalfTexel.x);", "external colour sampling does not hard-code an unpadded seam"),
        (scene, "    sampleUv.x = clamp(sampleUv.x, min(eyeMin, eyeMid), max(eyeMax, eyeMid));", "    sampleUv.x = clamp(sampleUv.x, eyeMin, eyeMax);", "external reversed-clamp guard"),
        (scene, "    sampleUv.y = clamp(sampleUv.y, uCrop.y, uCrop.y + uCrop.w);", "    sampleUv.y = clamp(sampleUv.y, uCrop.y + uHalfTexel.y, uCrop.y + uCrop.w - uHalfTexel.y);", "external colour sampling double inset"),
        (scene, "    if (footprint < lo) return vec4(g.x, g.x, g.w, g.w);", "    if (footprint < lo) return vec4(g.x, g.x, g.z, g.z);", "GSR left boundary gather component order (.w not .z)"),
        (scene, "    if (footprint >= hi - 1.0) return vec4(g.y, g.y, g.z, g.z);", "    if (footprint >= hi - 1.0) return vec4(g.y, g.y, g.w, g.w);", "GSR right boundary gather component order"),
        (scene, "vec2((base + 1.0) / width, uv.y)", "vec2((base + 0.5) / width, uv.y)", "GSR gather centre off a floor() discontinuity"),
        (scene, "base = clamp(base, lo, max(lo, hi - 2.0));", "base = clamp(base, lo, hi - 2.0);", "GSR reversed clamp guard"),
        (scene, "        float std = 2.181818 / max(sum, 1e-5);", "        float std = 2.181818 / sum;", "GSR zero-variance divide guard"),
        (scene, "    vec4 left = GatherEye(coord, eye);", "    vec4 left = textureGather(uTex, coord, mode);", "GSR initial gather eye isolation"),
        (app, "m_scene.RememberExternal(texture.texture, texture.width, texture.height, texture.crop, foveation);", "m_scene.RememberExternal(texture.texture,dec->Width(),texture.height,texture.crop,foveation);", "external snapshot uses logical dimensions"),
        (app, "m_scene.RememberExternal(texture.texture, texture.width, texture.height, texture.crop, foveation);", "m_scene.RememberExternal(dec->Geometry().texture,dec->TextureWidth(),dec->TextureHeight(),dec->Geometry().crop,foveation);", "external geometry mixes separately locked getters"),
        (decoder, "PublishTextureGeometry(Texture(), int(desc.width), int(desc.height), nextCrop);", "PublishTextureGeometry(Texture(), int(m_width), int(m_height), nextCrop);", "Android actual buffer dimensions"),
        (decoder, "PublishTextureGeometry(latched.texture, int(latched.width), int(latched.height), latched.crop);", "PublishTextureGeometry(latched.texture, int(m_width), int(m_height), latched.crop);", "Linux actual buffer dimensions"),
        (decoder, "PublishTextureGeometry(texture, 0, 0, crop);", "m_texture = texture;", "Android texture handle bypasses geometry publisher"),
        (decoder, "PublishTextureGeometry(texture, 0, 0, crop);", "m_texture = texture;", "Linux texture handle bypasses geometry publisher", 1),
        (decoder, "glGenTextures(1, &texture);", "glGenTextures(1, &m_texture);", "Android texture allocation bypasses local geometry publisher"),
        (decoder, "glGenTextures(1, &texture);", "glGenTextures(1, &m_texture);", "Linux texture allocation bypasses local geometry publisher", 1),
        (decoder, "if (geometry.texture) glDeleteTextures(1, &geometry.texture);", "if (m_texture) glDeleteTextures(1, &m_texture);", "Android shutdown bypasses geometry snapshot"),
        (decoder, "if (geometry.texture) glDeleteTextures(1, &geometry.texture);", "if (m_texture) glDeleteTextures(1, &m_texture);", "Linux shutdown bypasses geometry snapshot", 1),
        (decoder, "TextureGeometry Geometry() const {\n        std::lock_guard lock(m_geometryMutex);", "TextureGeometry Geometry() const {", "decoder geometry snapshot lock"),
        (decoder, "void VideoDecoder::PublishTextureGeometry(GLuint texture, int width, int height, const float crop[4]) {\n    std::lock_guard lock(m_geometryMutex);", "void VideoDecoder::PublishTextureGeometry(GLuint texture, int width, int height, const float crop[4]) {", "decoder geometry publisher lock"),
        (decoder, "    m_texture = texture;\n    m_textureWidth = width;", "    m_textureWidth = width;", "decoder geometry publisher omits texture handle"),
        (decoder, "void VideoDecoder::ResetTextureGeometry() {\n    const float crop[4] = {0, 0, 0, 0};\n    PublishTextureGeometry(0, 0, 0, crop);\n}", "void VideoDecoder::ResetTextureGeometry() {\n    m_texture = 0;\n    m_textureWidth = 0;\n    m_textureHeight = 0;\n    std::memset(m_crop, 0, sizeof(m_crop));\n}", "decoder geometry reset bypasses locked publisher"),
        (decoder, "    if (!desc.width || !desc.height) {", "    if (false) {", "empty Android hardware buffer rejection"),
        (decoder, "    auto geometry = Geometry();\n    ResetTextureGeometry();\n    if (geometry.texture) glDeleteTextures(1, &geometry.texture);", "    auto geometry = Geometry();\n    if (geometry.texture) glDeleteTextures(1, &geometry.texture);", "Android shutdown texture geometry reset"),
        (decoder, "    auto geometry = Geometry();\n    ResetTextureGeometry();\n    if (geometry.texture) glDeleteTextures(1, &geometry.texture);", "    auto geometry = Geometry();\n    if (geometry.texture) glDeleteTextures(1, &geometry.texture);", "Linux shutdown texture geometry reset", 1),
        (decoder, "    m_running = false;\n    m_cv.notify_all();\n    if (m_inputThread.joinable()) m_inputThread.join();\n    if (m_outputThread.joinable()) m_outputThread.join();", "    if (m_running.exchange(false)) {\n        m_cv.notify_all();\n        if (m_inputThread.joinable()) m_inputThread.join();\n        if (m_outputThread.joinable()) m_outputThread.join();\n    }", "V4L2 failed-worker shutdown join"),
        (decoder, "if (picture.image != EGL_NO_IMAGE_KHR) m_retiredImages.push_back(picture.image);", "", "V4L2 fail queues EGLImages instead of calling EGL on the output thread"),
        (decoder, "const GLuint texture = Geometry().texture;  // locked snapshot\n            PublishTextureGeometry(texture, 0, 0, zero);", "PublishTextureGeometry(0, 0, 0, zero);", "V4L2 fail keeps texture handle for Shutdown"),
        (decoder, "const uint64_t generation = ++m_batchGeneration;", "const uint64_t generation = 0;", "V4L2 batch generation must be monotonic"),
        (decoder, "m_batchGeneration == latched.generation &&\n                               m_pictures[size_t(index)].generation == latched.generation &&", "m_pictures[size_t(index)].fd == latched.fd &&", "V4L2 ABA guard uses generation not fd"),
        (decoder, "latched.texture = Geometry().texture;", "latched.texture = m_texture;", "V4L2 texture snapshot uses geometry lock"),
        (scene, "m_motionWidth > 0 ? 0.5f / float(m_motionWidth) : 0.f,\n                m_motionHeight > 0 ? 0.5f / float(m_motionHeight) : 0.f", "m_motionWidth > 0 ? 0.5f * fieldU / float(m_motionWidth) : 0.f,\n                m_motionHeight > 0 ? 0.5f * fieldV / float(m_motionHeight) : 0.f", "kept motion half-texel arithmetic"),
        (scene, "m_motionWidth > 0 ? 0.5f / float(m_motionWidth) : 0.f,\n                m_motionHeight > 0 ? 0.5f / float(m_motionHeight) : 0.f", "m_motionWidth > 0 ? 0.5f * fieldU / float(m_motionWidth) : 0.f,\n                m_motionHeight > 0 ? 0.5f * fieldV / float(m_motionHeight) : 0.f", "external motion half-texel arithmetic", 1),
        (scene, "vec2 Foveate(vec2 uv, float eye)", "vec2 Foveate(vec2 uv)", "explicit foveation eye argument"),
        (scene, "Foveate(vec2(vUv.x, 1.0 - vUv.y), step(0.5, vUv.x))", "Foveate(vec2(vUv.x, 1.0 - vUv.y), uv.x < 0.5 ? 0.0 : 1.0)", "blit linear foveation eye source"),
        (scene, "Foveate(vec2(vUv.x, 1.0 - vUv.y), step(0.5, vUv.x))", "Foveate(vec2(vUv.x, 1.0 - vUv.y), uv.x < 0.5 ? 0.0 : 1.0)", "blit gamma foveation eye source", 1),
        (scene, "Foveate(uv, eye)", "Foveate(uv, uv.x < 0.5 ? 0.0 : 1.0)", "moved foveation preserves raster eye"),
        (mailbox, "!gridWidth || !gridHeight || !blockSize || timing.intervalNs > timing.timestampNs ||\n        field.size() != size_t(gridWidth) * size_t(gridHeight) * 2", "false", "motion publish geometry and interval validation"),
        (mailbox, "const bool validLast = lastIntervalNs >= kMinMotionIntervalNs && lastIntervalNs <= kMaxMotionIntervalNs;", "const bool validLast = lastIntervalNs != 0;", "invalid previous motion interval rejection"),
        (mailbox, "return std::clamp(delta, kMinMotionIntervalNs, kMaxMotionIntervalNs);", "return delta;", "advancing motion period clamp"),
        (mailbox, "return std::clamp(uint64_t(currentSubmitNs - previousSubmitNs), kMinMotionIntervalNs, kMaxMotionIntervalNs);", "return std::max<uint64_t>(1'000'000ULL, uint64_t(currentSubmitNs - previousSubmitNs));", "submit gap motion period clamp"),
        (generation, "double(currentNs) < double(kMinMotionIntervalNs)", "false", "headset short motion period rejection"),
        (generation, "double(currentNs) > double(kMaxMotionIntervalNs)", "false", "headset long motion period rejection"),
        (generation, "const bool validPrevious = previousNs >= double(kMinMotionIntervalNs) && previousNs <= double(kMaxMotionIntervalNs);", "const bool validPrevious = previousNs > 0;", "headset EMA previous-period validation"),
        (app, "        if (id == rc::kVrStreamId) {\n            m_scene.ClearExternal();\n            m_vrHasFrame = m_vrLatched = m_motionMoves = m_motionConsistent = false;", "        if (id == rc::kVrStreamId) {\n            m_vrHasFrame = m_vrLatched = m_motionMoves = m_motionConsistent = false;", "VR decoder recovery clears stale external texture"),
        (app, "s.frameIndex = m_vrFrameIndex;", "s.frameIndex = ++m_zeroGenFrameIndex;", "zero-window trace must use wire seq, not a local counter"),
        (app, "s.trackingTimestampNs = m_vrMeta.trackingTimestampNs;", "s.trackingTimestampNs = 0;", "zero-window trace must carry the shared timestamp"),
        (decoder, "if (latched.generation != m_batchGeneration) {", "if (false) {", "V4L2 tail re-check of the batch generation"),
        (decoder, "++m_batchGeneration;", "++m_batchGenerationUnused;", "V4L2 fail invalidates in-flight latched copies"),
        (vr, "m_sender.RequestKeyframe();\n            }\n            continue;", "}\n            m_sender.RequestKeyframe();\n            continue;", "VR oversize keyframe request stays inside the throttle window"),
        (vr, "VisitDiagRing(end, count, uint32_t(kDiagRing), [&](uint32_t idx) {\n            frames.emplace_back(m_diagSeqRing[idx].load(std::memory_order_relaxed),\n                                m_diagEncUs[idx].load(std::memory_order_relaxed) / 1000.0);\n        });",
             "for (uint32_t i = end - count; i < end; i++)\n            frames.emplace_back(m_diagSeqRing[i % kDiagRing].load(std::memory_order_relaxed),\n                                m_diagEncUs[i % kDiagRing].load(std::memory_order_relaxed) / 1000.0);",
             "diag encode ring wraparound visit"),
        (vr, "const double newestAgeMs = ElapsedPositiveNs(now, motion.newestEnqueuedNs);",
             "const double newestAgeMs = motion.newestEnqueuedNs ? std::max(0.0, double(now) - double(motion.newestEnqueuedNs)) : 0.0;",
             "mailbox age 64-bit ns precision"),
        (vr, "const uint32_t converted = BoundedConvertedCount(given, bridge);",
             "const uint32_t converted = given - bridge;",
             "bounded converted frame count"),
    )
    def replace_nth(source, old, new, occurrence):
        parts = source.split(old)
        if len(parts) <= occurrence + 1:
            raise AssertionError(f"self-test mutation occurrence is missing: {old!r}")
        return old.join(parts[:occurrence + 1]) + new + old.join(parts[occurrence + 1:])

    sources = [scene, app, xr, xrh, vr, mailbox, decoder, sky, stream, generation, policy]
    for mutation in mutations:
        source, old, new, label = mutation[:4]
        occurrence = mutation[4] if len(mutation) > 4 else 0
        if old not in source:
            raise AssertionError(f"self-test mutation is missing: {label}")
        changed = sources.copy()
        changed[sources.index(source)] = replace_nth(source, old, new, occurrence)
        if not audit(*changed):
            raise AssertionError(f"contract gate failed to detect mutation: {label}")
    print(f"PASS: {len(mutations)} injected regressions were rejected")
    estimator_mutations = (
        (estimator, "const int left = blockOrigin.x < eyeW ? 0 : eyeW;", "const int left = corner.x + offset.x < eyeW ? 0 : eyeW;", "GPU candidate eye origin"),
        (estimator, "if (id.x >= uint(size.x / 32.0) || id.y >= uint(size.y / 32.0)) return;", "if (false) return;", "GPU coarse dispatch guard"),
        (estimator, "if (id.x >= uint(size.x / 32.0) || id.y >= uint(size.y / 32.0)) return;", "if (false) return;", "GPU refine dispatch guard", 1),
        (estimator, "p.x < left || p.x >= right || q.x < left || q.x >= right", "false", "GPU coarse same-eye candidate guard"),
        (estimator, "sad * (16.0 / float(count))", "sad", "GPU coarse valid-sample normalization"),
        (estimator, "n.x < 0 || n.y < 0", "false", "GPU refine negative neighbour rejection"),
        (estimator, "(n.x < int(eyeBlocks)) != (int(id.x) < int(eyeBlocks))", "false", "GPU refine seam neighbour guard"),
        (estimator, "if (id.x >= dimX || id.y >= dimY) return;", "if (false) return;", "GPU median dispatch guard"),
        (estimator, "if (exponent == 31) {", "if (false) {", "FP16 NaN/Inf decode"),
    )
    for mutation in estimator_mutations:
        source, old, new, label = mutation[:4]
        occurrence = mutation[4] if len(mutation) > 4 else 0
        if old not in source:
            raise AssertionError(f"self-test mutation is missing: {label}")
        if not audit_estimator(replace_nth(source, old, new, occurrence)):
            raise AssertionError(f"contract gate failed to detect mutation: {label}")
    print(f"PASS: {len(estimator_mutations)} injected estimator regressions were rejected")
    foveator_mutations = (
        (foveator, "eye * 0.5 + 0.5 * texel.x, eye * 0.5 + 0.5 - 0.5 * texel.x",
         "eye * 0.5 + texel.x, eye * 0.5 + 0.5 - texel.x", "PC Foveator whole-texel eye inset"),
        (foveator, "float2 uv = EyeUv(float2((ux + eye) * 0.5, uy), eye);", "float2 uv = float2((ux + eye) * 0.5, uy);", "PC Foveator centre clamp"),
        (foveator, "EyeUv(uv + float2(-o.x, -o.y), eye)", "uv + float2(-o.x, -o.y)", "PC Foveator top-left tap clamp"),
        (foveator, "EyeUv(uv + float2(o.x, -o.y), eye)", "uv + float2(o.x, -o.y)", "PC Foveator top-right tap clamp"),
        (foveator, "EyeUv(uv + float2(-o.x, o.y), eye)", "uv + float2(-o.x, o.y)", "PC Foveator bottom-left tap clamp"),
        (foveator, "EyeUv(uv + float2(o.x, o.y), eye)", "uv + float2(o.x, o.y)", "PC Foveator bottom-right tap clamp"),
    )
    for source, old, new, label in foveator_mutations:
        if old not in source:
            raise AssertionError(f"self-test mutation is missing: {label}")
        if not audit_foveator(source.replace(old, new, 1)):
            raise AssertionError(f"contract gate failed to detect mutation: {label}")
    print(f"PASS: {len(foveator_mutations)} injected Foveator regressions were rejected")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    scene = (REPO / SCENE).read_text(encoding="utf-8")
    app = (REPO / APP).read_text(encoding="utf-8")
    xr = (REPO / XR).read_text(encoding="utf-8")
    xrh = (REPO / XRH).read_text(encoding="utf-8")
    vr = (REPO / VR).read_text(encoding="utf-8")
    estimator = (REPO / ESTIMATOR).read_text(encoding="utf-8")
    estimator += "\n" + (REPO / ESTIMATOR_H).read_text(encoding="utf-8")
    foveator = (REPO / FOVEATOR).read_text(encoding="utf-8")
    mailbox = (REPO / MAILBOX).read_text(encoding="utf-8")
    decoder = (REPO / DECODER).read_text(encoding="utf-8")
    decoder += "\n" + (REPO / DECODER_H).read_text(encoding="utf-8")
    decoder += "\n" + (REPO / DECODER_V4L2).read_text(encoding="utf-8")
    decoder += "\n" + (REPO / DECODER_COMMON).read_text(encoding="utf-8")
    sky = (REPO / SKY).read_text(encoding="utf-8")
    stream = (REPO / STREAM).read_text(encoding="utf-8")
    generation = (REPO / GENERATION).read_text(encoding="utf-8")
    policy = (REPO / POLICY).read_text(encoding="utf-8")
    sources = [scene, app, xr, xrh, vr, mailbox, decoder, sky, stream, generation, policy]
    errors = audit(*sources) + audit_estimator(estimator) + audit_foveator(foveator)
    if errors:
        for error in errors:
            print(f"FAIL: {error}")
        return 1
    print("PASS: frame-generation/OpenXR static source contracts")
    print("Scope: source contracts plus injected-regression checks; OpenXR, compositor and visual smoothness were not executed")
    if args.self_test:
        self_test(*sources, estimator, foveator)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
