#include <qumir/parser/lexer_base.h>

namespace NQqb {
namespace NSql {

class TTokenStream : public NQumir::NAst::ITokenStream {
public:
    explicit TTokenStream(std::istream& in);

private:
    void Read() override;
};

} // namespace NSql
} // namespace NQqb
