#include "drape/vulkan/vulkan_memory_manager.hpp"
#include "drape/vulkan/vulkan_utils.hpp"

#include "base/assert.hpp"
#include "base/macros.hpp"
#include "base/math.hpp"

#include <algorithm>
#include <limits>

namespace dp
{
namespace vulkan
{
namespace
{
std::array<uint32_t, VulkanMemoryManager::kResourcesCount> const kMinBlockSizeInBytes =
{{
  1024 * 1024,  // Geometry
  0,            // Uniform (no minimal size)
  0,            // Staging (no minimal size)
  0,            // Image (no minimal size)
}};

std::array<uint32_t, VulkanMemoryManager::kResourcesCount> const kDesiredSizeInBytes =
{{
  80 * 1024 * 1024,                      // Geometry
  std::numeric_limits<uint32_t>::max(),  // Uniform (unlimited)
  20 * 1024 * 1024,                      // Staging
  std::numeric_limits<uint32_t>::max(),  // Image (unlimited)
}};

VkMemoryPropertyFlags GetMemoryPropertyFlags(VulkanMemoryManager::ResourceType resourceType,
                                             boost::optional<VkMemoryPropertyFlags> & fallbackTypeBits)
{
  switch (resourceType)
  {
  case VulkanMemoryManager::ResourceType::Geometry:
    fallbackTypeBits = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

  case VulkanMemoryManager::ResourceType::Staging:
    // No fallback.
    return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  case VulkanMemoryManager::ResourceType::Uniform:
    // No fallback.
    return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  case VulkanMemoryManager::ResourceType::Image:
    // No fallback.
    return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  case VulkanMemoryManager::ResourceType::Count:
    CHECK(false, ());
  }
  return 0;
}

bool Less(drape_ptr<VulkanMemoryManager::MemoryBlock> const & b1,
          drape_ptr<VulkanMemoryManager::MemoryBlock> const & b2)
{
  return b1->m_blockSize < b2->m_blockSize;
}
}  // namespace

VulkanMemoryManager::VulkanMemoryManager(VkDevice device, VkPhysicalDeviceLimits const & deviceLimits,
                                         VkPhysicalDeviceMemoryProperties const & memoryProperties)
  : m_device(device)
  , m_deviceLimits(deviceLimits)
  , m_memoryProperties(memoryProperties)
{}

VulkanMemoryManager::~VulkanMemoryManager()
{
  for (size_t i = 0; i < kResourcesCount; ++i)
  {
    for (auto const & b : m_freeBlocks[i])
      vkFreeMemory(m_device, b->m_memory, nullptr);

    for (auto const & p : m_memory[i])
    {
      for (auto const & b : p.second)
        vkFreeMemory(m_device, b->m_memory, nullptr);
    }
  }
}

boost::optional<uint32_t> VulkanMemoryManager::GetMemoryTypeIndex(uint32_t typeBits,
                                                                  VkMemoryPropertyFlags properties) const
{
  for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; i++)
  {
    if ((typeBits & 1) == 1)
    {
      if ((m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
        return i;
    }
    typeBits >>= 1;
  }
  return {};
}

uint32_t VulkanMemoryManager::GetOffsetAlignment(ResourceType resourceType) const
{
  if (resourceType == ResourceType::Uniform)
  {
    static uint32_t const kUniformAlignment =
        base::LCM(static_cast<uint32_t>(m_deviceLimits.minUniformBufferOffsetAlignment),
                  static_cast<uint32_t>(m_deviceLimits.nonCoherentAtomSize));
    return kUniformAlignment;
  }

  static uint32_t const kAlignment =
      base::LCM(static_cast<uint32_t>(m_deviceLimits.minMemoryMapAlignment),
                static_cast<uint32_t>(m_deviceLimits.nonCoherentAtomSize));
  return kAlignment;
}

uint32_t VulkanMemoryManager::GetSizeAlignment(VkMemoryRequirements const & memReqs) const
{
  return base::LCM(static_cast<uint32_t>(memReqs.alignment),
                   static_cast<uint32_t>(m_deviceLimits.nonCoherentAtomSize));
}

// static
uint32_t VulkanMemoryManager::GetAligned(uint32_t value, uint32_t alignment)
{
  if (alignment == 0)
    return value;
  return (value + alignment - 1) & ~(alignment - 1);
}

VulkanMemoryManager::AllocationPtr VulkanMemoryManager::Allocate(ResourceType resourceType,
                                                                 VkMemoryRequirements memReqs,
                                                                 uint64_t blockHash)
{
  auto const alignedSize = GetAligned(static_cast<uint32_t>(memReqs.size), GetSizeAlignment(memReqs));
  // Looking for an existed block.
  {
    auto & m = m_memory[static_cast<size_t>(resourceType)];
    auto const it = m.find(blockHash);
    if (it != m.end())
    {
      CHECK(!it->second.empty(), ());
      auto & block = it->second.back();
      auto const alignedOffset = GetAligned(block->m_freeOffset, GetOffsetAlignment(resourceType));

      // There is space in the current block.
      if (!block->m_isBlocked && (block->m_blockSize >= alignedOffset + alignedSize))
      {
        block->m_freeOffset = alignedOffset + alignedSize;
        block->m_allocationCounter++;
        return std::make_shared<Allocation>(resourceType, blockHash, alignedOffset, alignedSize,
                                            make_ref(block));
      }
    }

    // Looking for a block in free ones.
    auto & fm = m_freeBlocks[static_cast<size_t>(resourceType)];
    // Free blocks array must be sorted by size.
    static drape_ptr<MemoryBlock> refBlock = make_unique_dp<MemoryBlock>();
    refBlock->m_blockSize = alignedSize;
    auto const freeBlockIt = std::upper_bound(fm.begin(), fm.end(), refBlock, &Less);
    if (freeBlockIt != fm.end())
    {
      drape_ptr<MemoryBlock> freeBlock = std::move(*freeBlockIt);
      CHECK_EQUAL(freeBlock->m_allocationCounter, 0, ());
      CHECK_EQUAL(freeBlock->m_freeOffset, 0, ());
      CHECK_LESS_OR_EQUAL(alignedSize, freeBlock->m_blockSize, ());
      CHECK(!freeBlock->m_isBlocked, ());
      fm.erase(freeBlockIt);

      freeBlock->m_freeOffset = alignedSize;
      freeBlock->m_allocationCounter++;
      auto p = std::make_shared<Allocation>(resourceType, blockHash, 0, alignedSize,
                                            make_ref(freeBlock));

      m[blockHash].push_back(std::move(freeBlock));
      return p;
    }
  }

  // Looking for memory index by memory properties.
  boost::optional<VkMemoryPropertyFlags> fallbackFlags;
  auto flags = GetMemoryPropertyFlags(resourceType, fallbackFlags);
  auto memoryTypeIndex = GetMemoryTypeIndex(memReqs.memoryTypeBits, flags);
  if (!memoryTypeIndex && fallbackFlags)
  {
    flags = fallbackFlags.value();
    memoryTypeIndex = GetMemoryTypeIndex(memReqs.memoryTypeBits, flags);
  }

  if (!memoryTypeIndex)
    CHECK(false, ("Unsupported memory allocation configuration."));

  // Create new memory block.
  auto const blockSize = std::max(kMinBlockSizeInBytes[static_cast<size_t>(resourceType)],
                                  alignedSize);
  VkDeviceMemory memory = {};
  VkMemoryAllocateInfo memAllocInfo = {};
  memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memAllocInfo.pNext = nullptr;
  memAllocInfo.allocationSize = blockSize;
  memAllocInfo.memoryTypeIndex = memoryTypeIndex.value();
  CHECK_VK_CALL(vkAllocateMemory(m_device, &memAllocInfo, nullptr, &memory));
  m_sizes[static_cast<size_t>(resourceType)] += blockSize;

  // Attach block.
  auto & m = m_memory[static_cast<size_t>(resourceType)];

  auto newBlock = make_unique_dp<MemoryBlock>();
  newBlock->m_memory = memory;
  newBlock->m_blockSize = blockSize;
  newBlock->m_freeOffset = alignedSize;
  newBlock->m_allocationCounter++;
  newBlock->m_isCoherent = ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0);

  auto p = std::make_shared<Allocation>(resourceType, blockHash, 0, alignedSize,
                                        make_ref(newBlock));
  m[blockHash].push_back(std::move(newBlock));
  return p;
}

void VulkanMemoryManager::BeginDeallocationSession()
{
  m_isInDeallocationSession = true;
  m_deallocationSessionMask = 0;
}

void VulkanMemoryManager::Deallocate(AllocationPtr ptr)
{
  CHECK(ptr, ());
  CHECK(!ptr->m_memoryBlock->m_isBlocked, ());
  auto const resourceIndex = static_cast<size_t>(ptr->m_resourceType);
  auto & m = m_memory[resourceIndex];
  auto const it = m.find(ptr->m_blockHash);
  CHECK(it != m.end(), ());
  auto blockIt = std::find_if(it->second.begin(), it->second.end(),
                              [&ptr](drape_ptr<MemoryBlock> const & b)
  {
    ASSERT(ptr->m_memoryBlock != nullptr, ());
    return b->m_memory == ptr->m_memoryBlock->m_memory;
  });
  CHECK(blockIt != it->second.end(), ());
  CHECK_GREATER((*blockIt)->m_allocationCounter, 0, ());
  (*blockIt)->m_allocationCounter--;

  if ((*blockIt)->m_allocationCounter == 0)
  {
    if (m_isInDeallocationSession)
    {
      // Here we set a bit in the deallocation mask to skip the processing of untouched
      // resource collections.
      m_deallocationSessionMask |= (1 << resourceIndex);
    }
    else
    {
      drape_ptr<MemoryBlock> memoryBlock = std::move(*blockIt);
      it->second.erase(blockIt);
      if (m_sizes[resourceIndex] > kDesiredSizeInBytes[resourceIndex])
      {
        CHECK_LESS_OR_EQUAL(memoryBlock->m_blockSize, m_sizes[resourceIndex], ());
        m_sizes[resourceIndex] -= memoryBlock->m_blockSize;
        vkFreeMemory(m_device, memoryBlock->m_memory, nullptr);
      }
      else
      {
        memoryBlock->m_freeOffset = 0;
        auto & fm = m_freeBlocks[resourceIndex];
        fm.push_back(std::move(memoryBlock));
        std::sort(fm.begin(), fm.end(), &Less);
      }
    }
  }
}

void VulkanMemoryManager::EndDeallocationSession()
{
  if (!m_isInDeallocationSession)
    return;

  m_isInDeallocationSession = false;

  for (size_t i = 0; i < kResourcesCount; ++i)
  {
    if (((m_deallocationSessionMask >> i) & 1) == 0)
      continue;

    auto & fm = m_freeBlocks[i];

    static std::vector<uint64_t> hashesToDelete;
    for (auto & p : m_memory[i])
    {
      auto & m = p.second;
      m.erase(std::remove_if(m.begin(), m.end(),
                             [this, &fm, i](drape_ptr<MemoryBlock> & b)
      {
        if (b->m_allocationCounter == 0)
        {
          if (m_sizes[i] > kDesiredSizeInBytes[i])
          {
            CHECK_LESS_OR_EQUAL(b->m_blockSize, m_sizes[i], ());
            m_sizes[i] -= b->m_blockSize;
            vkFreeMemory(m_device, b->m_memory, nullptr);
          }
          else
          {
            auto block = std::move(b);
            block->m_freeOffset = 0;
            fm.push_back(std::move(block));
          }
          return true;
        }
        return false;
      }), m.end());

      if (m.empty())
        hashesToDelete.push_back(p.first);
    }

    for (auto hash : hashesToDelete)
      m_memory[i].erase(hash);
    hashesToDelete.clear();

    std::sort(fm.begin(), fm.end(), &Less);
  }
}
}  // namespace vulkan
}  // namespace dp
