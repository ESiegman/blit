#!/bin/sh
# Vulkan's and Metal's validation messages in a run's output, as problems:
# the Khronos layer's text and JSON forms, GPU-assisted validation's place
# in a shader, Metal's API validation (an assertion, or logged) and shader
# validation; the same message every frame as one row, counted; and Enter
# on a VUID's row showing the spec's statement, from a validusage.json.
#
# The samples in tests/validation are real output. Vulkan's: the Khronos
# validation layers 1.4.363 (Arch's vulkan-validation-layers 1.4.363.1,
# RADV on an RX 7900 XT), from bad.c and gpuav.c there (oob.comp built by
# glslangValidator -g), run as each case below says. Metal's: macOS 27.2 on
# Apple silicon, from bad.m there. validusage.json is a few entries of
# Vulkan-Headers' 1.4.363 file (/usr/share/vulkan/registry on Arch), as
# they are there. The one-line form of layers before 1.3.290 is not
# installed anywhere here: its case is written from that form's documented
# shape (the task's own example of it), not run.
#
#   tests/validation.sh path/to/blit            run the cases
#   tests/validation.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/validation.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
export BLIT_RECENT_FILE="$dir/recent"
export HOME="$dir/home"
mkdir -p "$HOME"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
export BLIT_VALIDUSAGE="$here/validation/validusage.json"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT [LINES]: the last LINES (1) lines the run printed, joined by /.
run() {
    got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$F;frame;$1" 2>&1 >/dev/null | sed -e "s#$here#@#g" -e "s#$dir#~#g" | tail -"${2:-1}" | tr '\n' '/' | sed 's#/$##')
}

# A folder whose tasks print the samples (the shader beside them, for
# GPU-assisted validation's place).
F="$dir/app"
mkdir -p "$F"
cp "$here/validation/oob.comp" "$F/"
S="$here/validation"
{
    # VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT VK_LAYER_REPORT_FLAGS=error:warn:perf:info ./bad
    printf 'text:\n\t@cat %s/vk-text.txt\n' "$S"
    # The same, with VK_LAYER_MESSAGE_FORMAT_JSON=1.
    printf 'json:\n\t@cat %s/vk-json.txt\n' "$S"
    # VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_GPUAV_ENABLE=1 ./gpuav (and with VK_LAYER_MESSAGE_FORMAT_JSON=1)
    printf 'gpuav:\n\t@cat %s/vk-gpuav.txt\n' "$S"
    printf 'gpuavjson:\n\t@cat %s/vk-gpuav-json.txt\n' "$S"
    # MTL_DEBUG_LAYER=1 ./bad api; MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog ./bad api 3;
    # MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 ./bad shader
    printf 'assert:\n\t@cat %s/mtl-assert.txt\n' "$S"
    printf 'nslog:\n\t@cat %s/mtl-nslog.txt\n' "$S"
    printf 'shader:\n\t@cat %s/mtl-shader.txt\n' "$S"
    # The text form's first messages, a second later the rest (the repeats of
    # one after its row was taken from the terminal).
    printf 'slow:\n\t@head -32 %s/vk-text.txt\n\t@sleep 1\n\t@tail -n +33 %s/vk-text.txt\n' "$S" "$S"
    printf 'oneline:\n\t@sh oneline.sh\n'
} >"$F/Makefile"
# Before 1.3.290 (written from the documented shape: see above), each on a
# line of its own, a messenger's words before it; with an object's name.
cat >"$F/oneline.sh" <<'EOF'
echo 'validation layer: Validation Error: [ VUID-vkCmdWaitEvents2-dependencyFlags-10394 ] Object 0: handle = 0x5566778899aa, name = frame commands, type = VK_OBJECT_TYPE_COMMAND_BUFFER; | MessageID = 0x1a2b3c4d | vkCmdWaitEvents2(): pDependencyInfos[0].dependencyFlags includes VK_DEPENDENCY_BY_REGION_BIT. The Vulkan spec states: The dependencyFlags member of any element of pDependencyInfo must not include any of the following bits (https://docs.vulkan.org/spec/latest/chapters/synchronization.html#VUID-vkCmdWaitEvents2-dependencyFlags-10394)'
echo 'validation layer: Validation Error: [ VUID-vkCmdWaitEvents2-dependencyFlags-10394 ] Object 0: handle = 0x5566778899bb, name = frame commands, type = VK_OBJECT_TYPE_COMMAND_BUFFER; | MessageID = 0x1a2b3c4d | vkCmdWaitEvents2(): pDependencyInfos[0].dependencyFlags includes VK_DEPENDENCY_BY_REGION_BIT. The Vulkan spec states: The dependencyFlags member of any element of pDependencyInfo must not include any of the following bits (https://docs.vulkan.org/spec/latest/chapters/synchronization.html#VUID-vkCmdWaitEvents2-dependencyFlags-10394)'
exit 1
EOF

vk='no file | >▸W vkCreateInstance(): Information with the layer settings provided: | E vkCreateBuffer(): pCreateInfo->size is zero. | W vkBindBufferMemory(): Trying to bind VkBuffer 0x20000000002 to a memory block which is fully consumed by the buffer. The required size of the allocation is 256, but smaller buffers like this should be sub-allocated from larger memory blocks. (Current threshold is 1048576 bytes) | ▸E vkCmdSetLineWidth(): lineWidth is 2.000000 (not 1.0), but the wideLines feature was not enabled. ×3 | ▸E vkFreeCommandBuffers(): pCommandBuffers[0] (VkCommandBuffer 0x5580ceb62780[frame commands]) is in use. | E vkDestroyFence(): can'"'"'t be called on VkFence 0x50000000005 that is currently in use by VkQueue 0x5580ceb82850'
run "buildrun:make text;buildwait;problemsdump"
check "the layer's text: a row each, no place, the Information left out, the line width every frame one row (x3)" "button: ▶ make text · 4 errors | Build 6 | $vk"
run "buildrun:make json;buildwait;problemsdump"
vkjson=$(printf '%s' "$vk" | sed -e 's/0x5580ceb62780/0x55c221b69fe0/' -e 's/0x5580ceb82850/0x55c221b48eb0/')
check "its JSON: the same rows" "button: ▶ make json · 4 errors | Build 6 | $vkjson"
run "buildrun:make slow;wait:600;problemsdump;buildwait;problemsdump" 2
check "said again after its row was taken: the row counts it" "button: ■ Stop · 2 errors | Build 4 | no file | >▸W vkCreateInstance(): Information with the layer settings provided: | E vkCreateBuffer(): pCreateInfo->size is zero. | W vkBindBufferMemory(): Trying to bind VkBuffer 0x20000000002 to a memory block which is fully consumed by the buffer. The required size of the allocation is 256, but smaller buffers like this should be sub-allocated from larger memory blocks. (Current threshold is 1048576 bytes) | ▸E vkCmdSetLineWidth(): lineWidth is 2.000000 (not 1.0), but the wideLines feature was not enabled./button: ▶ make slow · 4 errors | Build 6 | $vk"

gpuav='no file | >▸W vkCreateInstance(): Information with the layer settings provided: | ▸W vkCreateDevice(): Warning that validation is adjusting settings: | ▸W vkCreateDevice(): Warning that validation is adjusting settings: | file oob.comp | ▸E 7 vkCmdDispatch(): (set = 0, binding = 0, index 0)  access out of bounds. The descriptor buffer (VkBuffer 0x90000000009) size is 16 bytes, 16 bytes were bound, and the highest out of bounds access was at [259] bytes'
run "buildrun:make gpuav;buildwait;problemsdump"
check "GPU-assisted validation: at the shader's own line (the warnings, which say what they are about on the lines under them, a row each)" "button: ▶ make gpuav · 1 error | Build 4 | $gpuav"
run "buildrun:make gpuavjson;buildwait;problemsdump"
check "and in JSON" "button: ▶ make gpuavjson · 1 error | Build 4 | $gpuav"
run "buildrun:make gpuav;buildwait;key:cmd+shift+m;key:Down;key:Down;key:Down;key:Return;docdump:0"
check "Enter on it: its line in the shader" '> |    data.v[i] = 7;'

# Enter on a VUID's row (and the command for the one picked): the spec's
# statement, in a tab.
run "buildrun:make text;buildwait;key:cmd+shift+m;key:Down;key:Return;vuwait;docdump:8" 7
check "Enter: the spec's statement of its VUID" 'VUID-VkBufferCreateInfo-size-00912/> |VUID-VkBufferCreateInfo-size-00912/  VkBufferCreateInfo/  /  size must be greater than 0/  /  Vulkan 1.4.363, @/validation/validusage.json'
run "buildrun:make text;buildwait;key:cmd+shift+m;key:Down;key:Down;key:Down;key:Escape;command:Show the Vulkan spec's statement of the picked problem's VUID;vuwait;docdump:8" 7
check "the command: the picked one's" 'VUID-vkCmdSetLineWidth-lineWidth-00788/> |VUID-vkCmdSetLineWidth-lineWidth-00788/  vkCmdSetLineWidth/  /  If the wideLines feature is not enabled, lineWidth must be 1.0/  /  Vulkan 1.4.363, @/validation/validusage.json'
run "buildrun:make text;buildwait;key:cmd+shift+m;key:Return;command:Show the Vulkan spec's statement of the picked problem's VUID"
check "one with no VUID: none" 'blit: The problem picked names no VUID'

run "buildrun:make oneline;buildwait;problemsdump"
check "the one-line form: the message after its MessageID, up to the statement; the second (another handle) counted" 'button: ▶ make oneline · 1 error | Build 1 | no file | >▸E vkCmdWaitEvents2(): pDependencyInfos[0].dependencyFlags includes VK_DEPENDENCY_BY_REGION_BIT. ×2'
run "buildrun:make oneline;buildwait;key:cmd+shift+m;key:Return;vuwait;docdump:12" 12
check "a statement with a list: an item a line" 'VUID-vkCmdWaitEvents2-dependencyFlags-10394/> |VUID-vkCmdWaitEvents2-dependencyFlags-10394/  vkCmdWaitEvents2/  /  The dependencyFlags member of any element of pDependencyInfo must not include/  any of the following bits:/    • VK_DEPENDENCY_BY_REGION_BIT/    • VK_DEPENDENCY_DEVICE_GROUP_BIT/    • VK_DEPENDENCY_VIEW_LOCAL_BIT/    • VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT/  /  Vulkan 1.4.363, @/validation/validusage.json'

BLIT_VALIDUSAGE="$dir/none.json" run "buildrun:make text;buildwait;key:cmd+shift+m;key:Down;key:Return;vuwait"
check "no validusage.json: said so" 'blit: Cannot read ~/none.json'
head -c 300 "$S/validusage.json" >"$dir/cut.json"
BLIT_VALIDUSAGE="$dir/cut.json" run "buildrun:make text;buildwait;key:cmd+shift+m;key:Down;key:Return;vuwait"
check "one with no entries: said so" 'blit: ~/cut.json has no valid usage in it'
BLIT_VALIDUSAGE="$S/validusage.json" run "buildrun:make json;buildwait;key:cmd+shift+m;key:Down;key:Down;key:Down;key:Down;key:Down;key:Return;vuwait"
check "a VUID it does not have (the few here have not vkDestroyFence's): said so" 'blit: VUID-vkDestroyFence-fence-01120 is not in @/validation/validusage.json (Vulkan 1.4.363)'

# Metal.
run "buildrun:make assert;buildwait;problemsdump"
check "Metal's assertion" 'button: ▶ make assert · 1 error | Build 1 | no file | >E Draw Errors Validation: missing vertexDescriptor Buffer binding at index 0.'
run "buildrun:make nslog;buildwait;problemsdump"
check "logged, frame after frame: each thing wrong a row, counted" 'button: ▶ make nslog · 2 errors | Build 2 | no file | >E Draw Errors Validation: missing vertexDescriptor Buffer binding at index 0. ×3 | E Draw Errors Validation: Vertex Function(vmain): missing Buffer binding at index 0 for vertexBuffer.0[0]. ×3'
run "buildrun:make shader;buildwait;problemsdump"
check "shader validation: at its GPU stack's first frame (a library made at run time: program_source)" 'button: ▶ make shader · 1 error | Build 1 | file /program_source | >▸E 7:19 Invalid store in '"'"'device'"'"' or '"'"'constant'"'"' address space at offset 16384, executing compute function: "fill"'
run "buildrun:make assert;buildwait;key:cmd+shift+m;key:Return;command:Show the Vulkan spec's statement of the picked problem's VUID"
check "its row has no statement to show" 'blit: The problem picked names no VUID'

exit $failed
