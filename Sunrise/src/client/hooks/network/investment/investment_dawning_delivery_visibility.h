#pragma once
namespace sunrise::client::hooks::network::investment {
/** Read-only observation: match the current loaded tag8131908E and retained size.
 * Takes no patch lock, applies no repair, and does not pin the content lifetime. */
[[nodiscard]] bool is_dawning_delivery_vendor(const void* vendor) noexcept;
/** Before native vendor eligibility evaluation: bypass the retained FLAG779 event gate for branch
 * testing in the exact retained Zavala Give Gift interaction21 and its sale1 condition. Other
 * vendors are not supported. FLAG779's producer is unproved. Ownership828 remains native; no global
 * flag/account/save changes. */
void apply_dawning_delivery_visibility() noexcept;
/** Restore our instruction only while the same vendor and index handles remain loaded. */
void restore_dawning_delivery_visibility() noexcept;
} // namespace sunrise::client::hooks::network::investment
